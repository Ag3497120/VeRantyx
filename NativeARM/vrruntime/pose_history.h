// Bounded measured hand history for OpenVR's relative-time pose queries.
// No peak selection, velocity gain, or extrapolation of already predicted poses.
#pragma once
#include "openvr/openvr.h"
#include <array>
#include <deque>
#include <cmath>
#include <algorithm>

namespace vrrt {
class HandPoseHistory {
    struct Entry { double time; vr::TrackedDevicePose_t pose[2]; };
    std::deque<Entry> entries;
    static std::array<double,4> quaternion(const vr::HmdMatrix34_t &m) {
        std::array<double,4> q{};
        double trace=m.m[0][0]+m.m[1][1]+m.m[2][2];
        if(trace>0) {
            double s=2*std::sqrt(trace+1);q[3]=s/4;
            q[0]=(m.m[2][1]-m.m[1][2])/s;q[1]=(m.m[0][2]-m.m[2][0])/s;q[2]=(m.m[1][0]-m.m[0][1])/s;
        } else {
            int i=m.m[1][1]>m.m[0][0]?1:0;if(m.m[2][2]>m.m[i][i])i=2;
            int j=(i+1)%3,k=(i+2)%3;
            double s=2*std::sqrt(std::max(0.0,1.0+m.m[i][i]-m.m[j][j]-m.m[k][k]));
            if(s<1e-10)return {0,0,0,1};
            q[i]=s/4;q[j]=(m.m[j][i]+m.m[i][j])/s;q[k]=(m.m[k][i]+m.m[i][k])/s;q[3]=(m.m[k][j]-m.m[j][k])/s;
        }
        double n=0;for(double v:q)n+=v*v;n=std::sqrt(n);
        if(n<1e-10)return {0,0,0,1};for(auto &v:q)v/=n;return q;
    }
    static vr::HmdMatrix34_t rotation(std::array<double,4> a,std::array<double,4> b,double t) {
        double dot=0;for(int i=0;i<4;i++)dot+=a[i]*b[i];
        if(dot<0){for(auto &v:b)v=-v;dot=-dot;}
        double u=1-t,v=t;
        if(dot<.9995) {double angle=std::acos(std::clamp(dot,-1.0,1.0));double den=std::sin(angle);u=std::sin((1-t)*angle)/den;v=std::sin(t*angle)/den;}
        std::array<double,4> q{};double n=0;for(int i=0;i<4;i++){q[i]=u*a[i]+v*b[i];n+=q[i]*q[i];}
        n=std::sqrt(n);for(auto &value:q)value/=n;
        const double x=q[0],y=q[1],z=q[2],w=q[3];
        vr::HmdMatrix34_t m{};
        m.m[0][0]=1-2*(y*y+z*z);m.m[0][1]=2*(x*y-z*w);m.m[0][2]=2*(x*z+y*w);
        m.m[1][0]=2*(x*y+z*w);m.m[1][1]=1-2*(x*x+z*z);m.m[1][2]=2*(y*z-x*w);
        m.m[2][0]=2*(x*z-y*w);m.m[2][1]=2*(y*z+x*w);m.m[2][2]=1-2*(x*x+y*y);
        return m;
    }
public:
    void add(double time,const vr::TrackedDevicePose_t *poses) {
        if(!std::isfinite(time)||time<=0)return;
        if(!entries.empty()&&time<entries.back().time)entries.clear();
        if(!entries.empty()&&time==entries.back().time)return;
        Entry e{};e.time=time;e.pose[0]=poses[0];e.pose[1]=poses[1];entries.push_back(e);
        while(entries.size()>64 || time-entries.front().time>.5)entries.pop_front();
    }
    size_t size()const{return entries.size();}
    bool sample(int hand,double time,vr::TrackedDevicePose_t *out)const {
        if(!out||hand<0||hand>1||!std::isfinite(time)||entries.empty())return false;
        auto newer=std::lower_bound(entries.begin(),entries.end(),time,[](const Entry&e,double t){return e.time<t;});
        if(newer==entries.end()){*out=entries.back().pose[hand];return true;}
        if(newer==entries.begin()||newer->time==time){*out=newer->pose[hand];return true;}
        auto older=std::prev(newer);const auto&a=older->pose[hand];const auto&b=newer->pose[hand];
        if(!a.bPoseIsValid||!b.bPoseIsValid){*out=b;return true;}
        double t=(time-older->time)/(newer->time-older->time);*out=a;
        out->mDeviceToAbsoluteTracking=rotation(quaternion(a.mDeviceToAbsoluteTracking),quaternion(b.mDeviceToAbsoluteTracking),t);
        for(int i=0;i<3;i++) {
            out->mDeviceToAbsoluteTracking.m[i][3]=(float)((1-t)*a.mDeviceToAbsoluteTracking.m[i][3]+t*b.mDeviceToAbsoluteTracking.m[i][3]);
            out->vVelocity.v[i]=(float)((1-t)*a.vVelocity.v[i]+t*b.vVelocity.v[i]);
            out->vAngularVelocity.v[i]=(float)((1-t)*a.vAngularVelocity.v[i]+t*b.vAngularVelocity.v[i]);
        }
        return true;
    }
};
} // namespace vrrt
