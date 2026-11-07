#include "latest_decoder.hpp"
#include <cassert>
#include <chrono>
#include <atomic>
#include <random>
#include <iostream>
static const uint8_t I[]={0,0,0,1,40,1,128},P[]={0,0,0,1,2,1,128},E[]={0,0,0,1,6,2,128};
static std::atomic<unsigned>calls{0};static std::atomic<bool>fail{false};static std::atomic<bool>hold{false};
static bool submit(const uint8_t*,size_t,uint64_t,uint32_t){++calls;while(hold.load())std::this_thread::yield();return !fail.load();}
template<class F>void wait(F f){for(int i=0;i<2000;i++){if(f())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false);}
int main(){
 std::mt19937 rng(555);uint8_t bytes[64];for(int n=0;n<64;n++)for(int j=0;j<500;j++){for(auto&b:bytes)b=rng();mvr11::kind(bytes,n);}assert(!mvr11::kind(nullptr,99));
 void*c=(void*)123;
 {mvr11::LatestDecoder q(submit,1);q.new_epoch();q.resume(c);assert(q.offer(I,sizeof I,1000000,1));wait([&]{return calls==1;});
 assert(q.offer(P,sizeof P,2000000,2));assert(!q.offer(bytes,2,3000000,3));assert(!q.offer(P,sizeof P,4000000,4));assert(q.metrics().submitted==1);q.output(c,1000);assert(q.offer(I,sizeof I,5000000,5));wait([&]{return calls==2;});q.pause();}
 calls=0;
 {mvr11::LatestDecoder q(submit,1);q.new_epoch();q.resume(c);assert(q.offer(I,sizeof I,1000000,1));wait([&]{return calls==1;});for(int i=2;i<=5;i++)assert(q.offer(P,sizeof P,i*1000000ULL,i));assert(!q.offer(P,sizeof P,6000000,6));assert(q.metrics().overflow_resyncs==1);assert(!q.offer(E,sizeof E,7000000,7));assert(q.offer(I,sizeof I,8000000,8));q.output(c,1000);wait([&]{return calls==2;});q.pause();}
 calls=0;fail=true;
 {mvr11::LatestDecoder q(submit);q.new_epoch();q.resume(c);assert(q.offer(I,sizeof I,1000000,1));wait([&]{return q.metrics().failures==1;});assert(!q.offer(P,sizeof P,2000000,2));fail=false;assert(q.offer(I,sizeof I,3000000,3));wait([&]{return q.metrics().submitted==1;});q.pause();}
 calls=0;hold=true;
 {mvr11::LatestDecoder q(submit);q.new_epoch();q.resume(c);assert(q.offer(I,sizeof I,1000000,1));wait([&]{return calls==1;});std::atomic<bool>paused=false;std::thread t([&]{q.pause();paused=true;});std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(!paused);hold=false;t.join();assert(paused&&!q.active());q.new_epoch();q.resume(c);assert(!q.offer(P,sizeof P,2000000,2));q.pause();}
 std::cout<<"PASS: malformed parser fuzz, invalid-AU resync, bounded overflow, submit failure recovery, pause waits for in-flight call\n";
}
