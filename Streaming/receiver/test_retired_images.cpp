// SPDX-License-Identifier: MIT
#include "retired_images.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>
struct Image {uintptr_t cookie,buffer,planes;uint64_t pts;int32_t kind,pad;};
static_assert(sizeof(Image)==40,"Pinned arm64 DecodedImage layout");
using Queue=mvr10::RetiredImages<Image>;
using State=mvr10::FenceState;
using Outcome=mvr10::RetireOutcome;
struct Model {
 struct Own {int generation;bool complete=false;bool released=false;};
 std::map<uintptr_t,Own>owners;
 std::map<uintptr_t,State>fences;
 std::set<uintptr_t>deleted;
 std::map<int,unsigned>refs;
 unsigned release_count=0,finish_count=0,poll_count=0;
 bool finish_ok=true;
 Image image(unsigned id,int gen=1){owners[id]={gen,false,false};refs[gen]++;fences[id+1000]=State::Pending;return {id,id+100,id+200,100000+id,int32_t(0x80000000u|unsigned(gen)),0};}
 uintptr_t fence(unsigned id){return id+1000;}
 auto poll(){return [this](uintptr_t f){poll_count++;assert(!deleted.count(f));return fences.at(f);};}
 auto finish(){return [this](uintptr_t c){assert(c==11);finish_count++;if(!finish_ok)return false;for(auto &v:owners)v.second.complete=true;return true;};}
 auto release(){return [this](Image*i){auto &o=owners.at(i->cookie);assert(o.complete);assert(!o.released);assert(refs[o.generation]>0);refs[o.generation]--;o.released=true;release_count++;*i={};};}
 auto remove(){return [this](uintptr_t f){assert(!deleted.count(f));deleted.insert(f);};}
 void signal(unsigned id){owners.at(id).complete=true;fences[fence(id)]=State::Signaled;}
 mvr10::RetireResult put(Queue&q,Image&i,uintptr_t&f,uintptr_t c=11){return q.enqueue(i,f,c,finish(),release(),remove());}
 auto tick(Queue&q,uintptr_t c=11){return q.poll(c,poll(),finish(),release(),remove());}
 auto drain(Queue&q,uintptr_t c=11){return q.drain(c,finish(),release(),remove());}
};
int main(){unsigned cases=0;
 {Model m;Queue q;auto i=m.image(1);auto f=m.fence(1);auto r=m.put(q,i,f);assert(r.consumed&&q.size()==1&&!i.cookie&&!f);assert(m.refs[1]==1);m.tick(q);assert(m.release_count==0&&m.finish_count==0);m.signal(1);m.tick(q);assert(q.size()==0&&m.refs[1]==0&&m.release_count==1&&m.finish_count==0);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=2;n++){auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}m.signal(2);m.tick(q);assert(q.size()==1&&m.release_count==1);m.signal(1);m.tick(q);assert(q.size()==0&&m.release_count==2);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=3;n++){auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}m.fences[m.fence(2)]=State::Failed;auto r=m.tick(q);assert(r.outcome==Outcome::Drained&&q.size()==0&&m.release_count==3&&m.finish_count==1);cases++;}
 {Model m;Queue q;auto i=m.image(1);auto f=m.fence(1);m.put(q,i,f);m.fences[m.fence(1)]=State::Failed;m.finish_ok=false;auto r=m.tick(q);assert(r.outcome==Outcome::FinishFailedRetained&&q.failed()&&q.size()==1&&m.release_count==0&&m.deleted.empty());for(int n=0;n<100;n++)m.tick(q);assert(m.finish_count==1&&m.poll_count==1);auto j=m.image(2);auto g=m.fence(2);assert(!m.put(q,j,g).consumed&&j.cookie&&g);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=8;n++){auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}auto i=m.image(9);auto f=m.fence(9);auto r=m.put(q,i,f);assert(r.outcome==Outcome::OverflowDrained&&r.consumed&&!i.cookie&&!f&&q.size()==0);assert(m.release_count==9&&m.deleted.size()==9&&m.finish_count==1&&q.stats().high_water==8);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=8;n++){auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}m.finish_ok=false;auto i=m.image(9);const auto original=i;auto f=m.fence(9);auto r=m.put(q,i,f);assert(r.outcome==Outcome::FinishFailedRetained&&!r.consumed&&q.size()==8);assert(!std::memcmp(&i,&original,sizeof i)&&f==m.fence(9)&&m.release_count==0&&m.finish_count==1);cases++;}
 {Model m;Queue q;auto i=m.image(1);auto f=m.fence(1);m.put(q,i,f);auto j=m.image(2);auto g=m.fence(2);assert(m.put(q,j,g,22).outcome==Outcome::DifferentContext);assert(m.tick(q,22).outcome==Outcome::DifferentContext);assert(m.drain(q,22).outcome==Outcome::DifferentContext);assert(m.release_count==0&&m.finish_count==0&&m.poll_count==0&&j.cookie&&g);m.drain(q);assert(m.release_count==1);cases++;}
 {Model m;Queue q;auto i=m.image(1);i.kind=0;auto f=m.fence(1);assert(m.put(q,i,f).outcome==Outcome::InvalidPair&&i.cookie&&f);i.kind=int32_t(0x80000001u);i.buffer=0;assert(m.put(q,i,f).outcome==Outcome::InvalidPair);i.buffer=101;f=0;assert(m.put(q,i,f).outcome==Outcome::InvalidPair);f=m.fence(1);assert(m.put(q,i,f,0).outcome==Outcome::InvalidPair);assert(q.size()==0&&m.release_count==0);cases++;}
 {Model m;Queue q;auto i=m.image(1);auto alias=i;auto f=m.fence(1);m.put(q,i,f);auto aliasf=m.fence(1);assert(m.put(q,alias,aliasf).outcome==Outcome::DuplicateOwnership);auto j=m.image(2);assert(m.put(q,j,aliasf).outcome==Outcome::DuplicateOwnership);assert(q.size()==1&&m.release_count==0&&alias.cookie&&j.cookie);m.signal(1);m.tick(q);assert(m.release_count==1);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=4;n++){auto i=m.image(n,n<3?1:2);auto f=m.fence(n);m.put(q,i,f);}assert(m.refs[1]==2&&m.refs[2]==2);m.signal(1);m.signal(2);m.tick(q);assert(m.refs[1]==0&&m.refs[2]==2&&q.size()==2);m.drain(q);assert(m.refs[2]==0&&q.size()==0&&m.finish_count==1);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=8;n++){auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}assert(m.drain(q).outcome==Outcome::Drained&&m.finish_count==1&&m.release_count==8);assert(m.drain(q).outcome==Outcome::Empty&&m.finish_count==1);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=10000;n++){auto i=m.image(n,int(n%3));auto f=m.fence(n);m.put(q,i,f);if(n>3)m.signal(n-3);m.tick(q);assert(q.size()<=4);}m.drain(q);assert(m.release_count==10000&&m.deleted.size()==10000&&q.stats().high_water<=4);for(auto&r:m.refs)assert(r.second==0);cases++;}
 {Model m;Queue q;auto i=m.image(1);auto f=m.fence(1);auto r=q.prepare_slot(i,f,uintptr_t(11),m.finish(),m.release(),m.remove());assert(r.outcome==Outcome::SlotReady&&!r.consumed&&i.cookie&&f&&q.size()==0);assert(m.put(q,i,f).consumed);m.drain(q);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=8;n++){auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}auto i=m.image(9);auto f=m.fence(9);auto r=q.prepare_slot(i,f,uintptr_t(11),m.finish(),m.release(),m.remove());assert(r.outcome==Outcome::OverflowDrained&&!r.consumed&&i.cookie&&f&&q.size()==0&&m.finish_count==1&&m.release_count==8);assert(m.owners[9].complete);m.remove()(f);m.release()(&i);assert(m.release_count==9);cases++;}
 {Model m;Queue q;for(unsigned n=1;n<=8;n++){auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}auto i=m.image(9);auto f=m.fence(9);m.finish_ok=false;auto r=q.prepare_slot(i,f,uintptr_t(11),m.finish(),m.release(),m.remove());assert(r.outcome==Outcome::FinishFailedRetained&&!r.consumed&&i.cookie&&f&&q.size()==8&&m.finish_count==1&&m.release_count==0);cases++;}
 {Model m;Queue q;assert(q.can_accept(11)&&!q.can_accept(0));for(unsigned n=1;n<=8;n++){assert(q.can_accept(11));auto i=m.image(n);auto f=m.fence(n);m.put(q,i,f);}assert(!q.can_accept(11)&&!q.can_accept(22));std::set<uintptr_t>seen;const Queue&read=q;read.for_each_fence([&](uintptr_t f){assert(seen.insert(f).second);});assert(seen.size()==8&&q.size()==8&&m.release_count==0);m.drain(q);assert(q.can_accept(11)&&q.can_accept(22));cases++;}
 {Model m;Queue q;auto i=m.image(1);auto f=m.fence(1);m.put(q,i,f);assert(q.can_accept(11)&&!q.can_accept(22));m.finish_ok=false;m.fences[m.fence(1)]=State::Failed;m.tick(q);assert(!q.can_accept(11)&&!q.can_accept(22));cases++;}
 std::printf("{\"pass\":true,\"cases\":%u,\"stress_transfers\":10000,\"fixed_capacity\":8,\"context_mismatch_guard\":true,\"failed_finish_retains_ownership\":true}\n",cases);
}
