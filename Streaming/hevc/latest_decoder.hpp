// SPDX-License-Identifier: MIT
#pragma once
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <deque>
#include <array>
#include <cstdint>
#include <algorithm>
namespace mvr11 {
// Narrow, fail-closed parser for the pinned Apple two-sublayer encoder:
// 1=IDR at T0; 2=TRAIL at T0; 3=TSA at T1. TSA pictures cannot reference
// earlier T1 pictures; T0 pictures cannot reference T1. Other VCL is refused.
inline int kind(const uint8_t*p,size_t n){
 if(!p||n<7||n>16*1024*1024)return 0;int result=0;bool seen=false;size_t i=0;
 while(i+3<n){size_t pre=p[i]==0&&p[i+1]==0&&p[i+2]==1?3:(i+4<n&&p[i]==0&&p[i+1]==0&&p[i+2]==0&&p[i+3]==1?4:0);
  if(!pre){if(!seen&&p[i])return 0;++i;continue;}size_t h=i+pre;if(h+2>=n||(p[h]&128)||!(p[h+1]&7))return 0;unsigned type=(p[h]>>1)&63,tid=(p[h+1]&7)-1,layer=((p[h]&1)<<5)|(p[h+1]>>3);if(layer)return 0;
  if(type<32){int k=tid==0&& (type==19||type==20)?1:(tid==0&&(type==0||type==1)?2:(tid==1&&(type==2||type==3)?3:0));if(!k||(result&&result!=k))return 0;result=k;}seen=true;i=h+2;
 }return seen?result:0;
}
inline bool independent(const uint8_t*p,size_t n){return kind(p,n)==1;}
struct Packet{std::vector<uint8_t>data;uint64_t pts=0;uint32_t id=0;bool idr=false,enhancement=false;};
struct Metrics{uint64_t accepted=0,replaced=0,submitted=0,outputs=0,rejected=0,failures=0,cancelled=0,transitions=0,awaiting_idr_drops=0,enhancement_drops=0,overflow_resyncs=0;size_t high_water=0,pending_high_water=0;};
class LatestDecoder{
 public:
 using Submit=bool(*)(const uint8_t*,size_t,uint64_t,uint32_t);
 explicit LatestDecoder(Submit f,size_t credits=2):submit_(f),credits_(std::clamp<size_t>(credits,1,4)),worker_([this]{loop();}){}
 ~LatestDecoder(){std::unique_lock<std::mutex>l(m_);quit_=true;enabled_=false;pending_.clear();cv_.notify_all();l.unlock();worker_.join();}
 void pause(){std::unique_lock<std::mutex>l(m_);enabled_=false;metrics_.cancelled+=pending_.size();pending_.clear();await_idr_=true;cv_.notify_all();cv_.wait(l,[&]{return !busy_;});codec_=nullptr;slots_={};metrics_.transitions++;}
 void resume(void*c){std::lock_guard<std::mutex>l(m_);codec_=c;enabled_=c;cv_.notify_all();}
 bool active(){std::lock_guard<std::mutex>l(m_);return enabled_;}
 bool offer(const uint8_t*p,size_t n,uint64_t pts,uint32_t id){int k=kind(p,n);std::lock_guard<std::mutex>l(m_);
  if(!k){metrics_.rejected++;metrics_.cancelled+=pending_.size();pending_.clear();await_idr_=true;return false;}
  if(!enabled_||!pts||pts<=last_pts_){metrics_.rejected++;return false;}last_pts_=pts;
  if(k==1){metrics_.replaced+=pending_.size();pending_.clear();await_idr_=false;}
  else if(await_idr_){metrics_.awaiting_idr_drops++;return false;}
  else { // Never discard a T0 dependency to make room for a T1 picture.
   for(auto it=pending_.begin();it!=pending_.end();){if(it->enhancement){it=pending_.erase(it);metrics_.enhancement_drops++;metrics_.replaced++;}else ++it;}
   if(k==3&&pending_.size()>=2){metrics_.accepted++;metrics_.enhancement_drops++;metrics_.replaced++;return true;}
   if(pending_.size()>=4){metrics_.overflow_resyncs++;metrics_.replaced+=pending_.size();pending_.clear();await_idr_=true;metrics_.awaiting_idr_drops++;return false;}
  }
  Packet q;q.data.assign(p,p+n);q.pts=pts;q.id=id;q.idr=k==1;q.enhancement=k==3;pending_.push_back(std::move(q));metrics_.accepted++;metrics_.pending_high_water=std::max(metrics_.pending_high_water,pending_.size());cv_.notify_all();return true;
 }
 void output(void*c,int64_t us){std::lock_guard<std::mutex>l(m_);if(c!=codec_)return;for(auto&s:slots_)if(s.used&&int64_t(s.pts/1000)==us){s={};metrics_.outputs++;cv_.notify_all();break;}}
 Metrics metrics(){std::lock_guard<std::mutex>l(m_);return metrics_;}
 void new_epoch(){std::lock_guard<std::mutex>l(m_);last_pts_=0;await_idr_=true;}
 private:
 struct Slot{bool used=false;uint64_t pts=0;};
 size_t count(){size_t c=0;for(auto&s:slots_)c+=s.used;return c;}
 void loop(){std::unique_lock<std::mutex>l(m_);while(!quit_){cv_.wait(l,[&]{return quit_||(enabled_&&!pending_.empty()&&count()<credits_);});if(quit_)break;
  Packet q=std::move(pending_.front());pending_.pop_front();size_t ix=0;while(slots_[ix].used)ix++;slots_[ix]={true,q.pts};busy_=true;metrics_.high_water=std::max(metrics_.high_water,count());l.unlock();bool ok=submit_(q.data.data(),q.data.size(),q.pts,q.id);l.lock();busy_=false;
  if(ok)metrics_.submitted++;else{metrics_.failures++;if(slots_[ix].pts==q.pts)slots_[ix]={};if(q.enhancement){metrics_.enhancement_drops++;}else{while(!pending_.empty()&&!pending_.front().idr){pending_.pop_front();metrics_.replaced++;}await_idr_=pending_.empty();}}
  cv_.notify_all();
 }}
 Submit submit_;size_t credits_;std::mutex m_;std::condition_variable cv_;bool enabled_=false,quit_=false,busy_=false,await_idr_=true;void*codec_=nullptr;uint64_t last_pts_=0;std::deque<Packet>pending_;std::array<Slot,4>slots_{};Metrics metrics_;std::thread worker_;
};
}
