#include "latest_decoder.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
static std::mutex mutex;static std::vector<uint32_t>ids;
static bool submit(const uint8_t*,size_t,uint64_t,uint32_t id){std::lock_guard<std::mutex>l(mutex);ids.push_back(id);return true;}
static const uint8_t E[]={0,0,0,1,6,2,128};
static const uint8_t I[]={0,0,0,1,40,1,128},P[]={0,0,0,1,2,1,128},CRA[]={0,0,0,1,42,1,128};
static void waitsize(size_t n){for(int i=0;i<1000;i++){ {std::lock_guard<std::mutex>l(mutex);if(ids.size()>=n)return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false);}
int main(){assert(mvr11::kind(I,sizeof I)==1);assert(mvr11::kind(P,sizeof P)==2);assert(!mvr11::kind(CRA,sizeof CRA));mvr11::LatestDecoder q(submit);void*c=(void*)123;q.new_epoch();q.resume(c);assert(!q.offer(P,sizeof P,1000000,0));assert(q.offer(I,sizeof I,2000000,1));waitsize(1);assert(q.offer(P,sizeof P,3000000,2));waitsize(2);
 // Both decoder credits busy. The unsubmitted suffix P3/P4 may only be removed by I5.
 assert(q.offer(E,sizeof E,4000000,3));assert(q.offer(P,sizeof P,5000000,4));assert(q.offer(E,sizeof E,6000000,5));assert(q.offer(E,sizeof E,7000000,6));q.output(c,2000);waitsize(3);q.output(c,3000);waitsize(4);{std::lock_guard<std::mutex>l(mutex);assert((ids==std::vector<uint32_t>{1,2,4,6}));}
 assert(!q.offer(P,sizeof P,7000000,7));q.output(c,5000);q.output(c,7000);q.pause();auto m=q.metrics();assert(m.replaced==2&&m.high_water==2&&m.pending_high_water<=4);q.new_epoch();q.resume(c);assert(!q.offer(P,sizeof P,8000000,8));q.pause();std::cout<<"PASS: T0 retained, only TSA enhancements removed, bounded credits, strict PTS, CRA refusal, reset waits for IDR\n";}
