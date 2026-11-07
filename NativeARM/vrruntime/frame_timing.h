#pragma once
#include "openvr/openvr.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace vrrt {
// CPU timestamps only. GPU fields deliberately stay zero: queue latency, frame
// pacing and readback wall time are not GPU-busy measurements. Caller owns lock.
class FrameCpuTimings {
    vr::Compositor_FrameTiming current_{};
    std::deque<vr::Compositor_FrameTiming> completed_;
    double waitStart_ = 0, posesReady_ = 0;
    bool active_ = false;
public:
    void clear() { current_ = {}; completed_.clear(); active_ = false; }
    void begin(uint32_t id, double waitStart, double posesReady,
               const vr::TrackedDevicePose_t &pose) {
        if (!std::isfinite(waitStart) || !std::isfinite(posesReady) || posesReady < waitStart) return;
        if (active_ && waitStart >= waitStart_) {
            current_.m_flClientFrameIntervalMs = float((waitStart-waitStart_)*1000.);
            completed_.push_back(current_);
            if (completed_.size() > 64) completed_.pop_front();
        } else if (active_) completed_.clear(); // clock reset: discard incompatible history
        current_ = {};
        current_.m_nSize = sizeof(current_);
        current_.m_nFrameIndex = id;
        current_.m_flSystemTimeInSeconds = posesReady;
        current_.m_flWaitGetPosesCalledMs = float((waitStart-posesReady)*1000.);
        current_.m_HmdPose = pose;
        waitStart_ = waitStart; posesReady_ = posesReady; active_ = true;
    }
    void submit(double seconds, double finished, bool stereoReady) {
        if (!active_ || !std::isfinite(seconds) || !std::isfinite(finished) || seconds < 0) return;
        current_.m_flSubmitFrameMs += float(seconds*1000.);
        if (stereoReady && finished >= posesReady_)
            current_.m_flNewFrameReadyMs = float((finished-posesReady_)*1000.);
        // Actual headset presents/repeats are not available through this bridge.
        // Do not manufacture present/drop counts from the nominal refresh rate.
    }
    bool get(vr::Compositor_FrameTiming *out, uint32_t ago) const {
        if (!out) return false;
        uint32_t size; std::memcpy(&size, static_cast<const void *>(out), sizeof(size));
        if (size < sizeof(uint32_t) || ago >= completed_.size()) return false;
        auto value = completed_[completed_.size()-1-ago]; value.m_nSize = size;
        std::memcpy(out, &value, std::min<size_t>(size, sizeof(value)));
        return true;
    }
    uint32_t range(vr::Compositor_FrameTiming *out, uint32_t count) const {
        if (!out) return 0;
        uint32_t size; std::memcpy(&size, static_cast<const void *>(out), sizeof(size));
        if (size < sizeof(uint32_t)) return 0;
        count = std::min<size_t>(count, completed_.size());
        auto *bytes = reinterpret_cast<unsigned char *>(out);
        for (uint32_t i=0; i<count; ++i) {
            auto value = completed_[completed_.size()-count+i]; value.m_nSize = size;
            std::memcpy(bytes+size_t(i)*size, &value, std::min<size_t>(size, sizeof(value)));
        }
        return count;
    }
};
}
