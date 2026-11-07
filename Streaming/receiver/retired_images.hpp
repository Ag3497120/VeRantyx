// SPDX-License-Identifier: MIT
// Fixed-capacity ownership only; GL calls and library entrypoints are callbacks.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mvr10 {
enum class FenceState { Pending, Signaled, Failed };
enum class RetireOutcome {
 Queued, Empty, SlotReady, Polled, Drained, OverflowDrained,
 InvalidPair, DifferentContext, DuplicateOwnership, FinishFailedRetained
};
struct RetireResult {
 RetireOutcome outcome=RetireOutcome::Empty;
 bool consumed=false; // true only if this call took/cleared an incoming pair
 std::size_t pending=0;
};
struct RetireStats {
 uint64_t queued=0, released=0, deleted_fences=0, polls=0, finishes=0;
 uint64_t failed_waits=0, failed_finishes=0, overflows=0;
 std::size_t high_water=0;
};

// Single-render-thread object. Image must be the pinned 40-byte DecodedImage POD
// (cookie, buffer, planes, pts, signed kind, pad). No additional acquire/ref bump
// is needed: enqueue TRANSFERS the exact existing cookie and image-slot ownership.
// The queue must not outlive its image-release library or owning GL context.
//
// poll_fence(Fence) must issue a NONBLOCKING wait on this same current context.
// finish(Context) must prove all its earlier GL reads completed and return true;
// an error or lost/wrong context must return false. Never equate an error with
// completion. release_image(Image*) must be the REAL pwReleaseImage/decReleaseImage
// and delete_fence(Fence) the REAL GL delete, bypassing interception recursion.
// None of the callbacks may throw or reenter this queue.
template<class Image, class Fence=uintptr_t, class Context=uintptr_t,
         std::size_t Capacity=8>
class RetiredImages {
 static_assert(Capacity>0,"Retirement storage must be bounded and nonempty");
 static_assert(std::is_trivially_copyable<Image>::value,"Image must be POD");
 struct Entry { Image image{}; Fence fence{}; };
 std::array<Entry,Capacity> entries_{};
 std::size_t count_=0;
 Context context_{};
 bool finish_failed_=false;
 RetireStats stats_{};

 static bool owns(const Image&i) {
  return i.cookie && (uint32_t(i.kind)&0x80000000u);
 }
 RetireResult result(RetireOutcome out,bool consumed=false) const {
  return {out,consumed,count_};
 }
 template<class Release,class Delete>
 void dispose(Entry &e,Release &release_image,Delete &delete_fence) {
  // Reaching here requires a signaled fence or successful same-context finish.
  delete_fence(e.fence);stats_.deleted_fences++;
  release_image(&e.image);stats_.released++;
  e=Entry{};
 }
 template<class Release,class Delete>
 void dispose_all(Release &release_image,Delete &delete_fence) {
  for(std::size_t i=0;i<count_;i++)dispose(entries_[i],release_image,delete_fence);
  count_=0;context_=Context{};finish_failed_=false;
 }
 template<class Finish>
 bool prove_finished(Context current,Finish &finish) {
  // A failed finish latches the exceptional state. Do not add glFinish to every
  // later frame after context loss. Integration must stop/teardown that context;
  // it may intentionally retain these few ownership refs until process exit.
  if(finish_failed_)return false;
  stats_.finishes++;
  if(finish(current))return true;
  finish_failed_=true;stats_.failed_finishes++;return false;
 }
public:
 RetiredImages()=default;
 RetiredImages(const RetiredImages&)=delete;
 RetiredImages&operator=(const RetiredImages&)=delete;
 // The destructor deliberately performs NO GL calls/releases. Explicit drain on
 // the current render context is required; speculative release would be unsafe.
 std::size_t size()const{return count_;}
 Context context()const{return context_;}
 bool failed()const{return finish_failed_;}
 const RetireStats &stats()const{return stats_;}
 bool can_accept(Context current)const {
  return current!=Context{} && !finish_failed_ && count_<Capacity &&
         (!count_||current==context_);
 }
 // Read-only visitation supports proving completion of EVERY retained sync after
 // a real finish. The callback must not mutate/reenter the queue. This neither
 // transfers ownership nor changes the list while a finish callback validates it.
 template<class Visit>
 void for_each_fence(Visit visit)const {
  for(std::size_t i=0;i<count_;i++)visit(entries_[i].fence);
 }

 // Optional PRE-DELETE integration gate. Call on the same render thread before
 // suppressing the original glDeleteSync. SlotReady guarantees enqueue will fit
 // if no callbacks reenter/mutate the queue and context/image identity stays the
 // same. OverflowDrained proves completion of the incoming fence too, but leaves
 // its pair untouched: integration can allow the original delete/release path.
 // Any other outcome must NOT fall back to releasing an unfinished incoming
 // image. In particular a lost-context failure needs quarantine + stopped draw.
 template<class Finish,class Release,class Delete>
 RetireResult prepare_slot(const Image &source,Fence fence,Context current,
                          Finish finish,Release release_image,Delete delete_fence) {
  if(!owns(source)||!source.buffer||fence==Fence{}||current==Context{})
   return result(RetireOutcome::InvalidPair);
  if(count_ && current!=context_)return result(RetireOutcome::DifferentContext);
  for(std::size_t i=0;i<count_;i++)
   if(entries_[i].image.cookie==source.cookie||entries_[i].fence==fence)
    return result(RetireOutcome::DuplicateOwnership);
  if(finish_failed_)return result(RetireOutcome::FinishFailedRetained);
  if(count_<Capacity)return result(RetireOutcome::SlotReady);
  stats_.overflows++;
  if(!prove_finished(current,finish))return result(RetireOutcome::FinishFailedRetained);
  dispose_all(release_image,delete_fence);
  return result(RetireOutcome::OverflowDrained);
 }

 template<class Finish,class Release,class Delete>
 RetireResult enqueue(Image &source,Fence &fence,Context current,Finish finish,
                      Release release_image,Delete delete_fence) {
  if(!owns(source)||!source.buffer||fence==Fence{}||current==Context{})
   return result(RetireOutcome::InvalidPair);
  if(count_ && current!=context_)return result(RetireOutcome::DifferentContext);
  for(std::size_t i=0;i<count_;i++)
   if(entries_[i].image.cookie==source.cookie||entries_[i].fence==fence)
    return result(RetireOutcome::DuplicateOwnership);
  if(finish_failed_)return result(RetireOutcome::FinishFailedRetained);
  if(count_==Capacity){
   stats_.overflows++;
   if(!prove_finished(current,finish))return result(RetireOutcome::FinishFailedRetained);
   // This finish also covers the incoming old-history fence. Release it now;
   // retaining it until another frame would serve no ownership purpose.
   dispose_all(release_image,delete_fence);
   Entry incoming{source,fence};source=Image{};fence=Fence{};
   dispose(incoming,release_image,delete_fence);
   return result(RetireOutcome::OverflowDrained,true);
  }
  context_=current;
  entries_[count_++]={source,fence};source=Image{};fence=Fence{};
  stats_.queued++;if(count_>stats_.high_water)stats_.high_water=count_;
  return result(RetireOutcome::Queued,true);
 }

 template<class Poll,class Finish,class Release,class Delete>
 RetireResult poll(Context current,Poll poll_fence,Finish finish,
                   Release release_image,Delete delete_fence) {
  if(!count_)return result(RetireOutcome::Empty);
  if(current==Context{}||current!=context_)return result(RetireOutcome::DifferentContext);
  if(finish_failed_)return result(RetireOutcome::FinishFailedRetained);
  stats_.polls++;
  std::size_t i=0;
  while(i<count_){
   const FenceState state=poll_fence(entries_[i].fence);
   if(state==FenceState::Pending){i++;continue;}
   if(state==FenceState::Failed){
    stats_.failed_waits++;
    if(!prove_finished(current,finish))return result(RetireOutcome::FinishFailedRetained);
    dispose_all(release_image,delete_fence);return result(RetireOutcome::Drained);
   }
   dispose(entries_[i],release_image,delete_fence);
   for(std::size_t j=i+1;j<count_;j++)entries_[j-1]=entries_[j];
   entries_[--count_]=Entry{};
  }
  if(!count_)context_=Context{};
  return result(RetireOutcome::Polled);
 }

 // Teardown only, before unbinding/destroying the current owning GL context and
 // BEFORE unloading/resetting libraries that service the retained image cookies.
 template<class Finish,class Release,class Delete>
 RetireResult drain(Context current,Finish finish,Release release_image,
                    Delete delete_fence) {
  if(!count_)return result(RetireOutcome::Empty);
  if(current==Context{}||current!=context_)return result(RetireOutcome::DifferentContext);
  if(!prove_finished(current,finish))return result(RetireOutcome::FinishFailedRetained);
  dispose_all(release_image,delete_fence);return result(RetireOutcome::Drained);
 }
};
} // namespace mvr10
