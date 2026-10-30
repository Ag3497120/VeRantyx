// Native Source 2's SDR output is brighter than the Windows reference. Match
// its shadow/midtone exposure in linear light, keeping black and white fixed.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace vrrt {

class NativeDisplayCurve {
    std::array<uint8_t, 256> m_table;
    float m_exposure;
public:
    explicit NativeDisplayCurve(float exposure = -1.f) : m_exposure(exposure) {
        if (!std::isfinite(m_exposure) || m_exposure < -3.f || m_exposure > 3.f) m_exposure = -1.f;
        const double gain = std::exp2(m_exposure);
        for (unsigned i = 0; i < m_table.size(); i++) {
            if (m_exposure == 0) { m_table[i] = uint8_t(i); continue; }
            double v = i / 255.0;
            v = v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
            // Normalize exposure around display white: lamps must retain their
            // bright cores rather than turning grey with a simple RGB gain.
            v = gain * v / (1 + (gain - 1) * v);
            v = v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - .055;
            m_table[i] = uint8_t(std::lround(v * 255));
        }
    }
    float exposure() const { return m_exposure; }
    uint8_t map(uint8_t value) const { return m_table[value]; }

    void apply_bgra(uint8_t *pixels, size_t count) const {
        if (m_exposure == 0) return;
#if defined(__aarch64__)
        // The worker already copies full stereo frames. A byte lookup avoids
        // per-pixel powers and keeps the extra pass within its frame budget.
        const uint8x16x4_t t0 = {{vld1q_u8(m_table.data()), vld1q_u8(m_table.data()+16),
                                vld1q_u8(m_table.data()+32), vld1q_u8(m_table.data()+48)}};
        const uint8x16x4_t t1 = {{vld1q_u8(m_table.data()+64), vld1q_u8(m_table.data()+80),
                                vld1q_u8(m_table.data()+96), vld1q_u8(m_table.data()+112)}};
        const uint8x16x4_t t2 = {{vld1q_u8(m_table.data()+128), vld1q_u8(m_table.data()+144),
                                vld1q_u8(m_table.data()+160), vld1q_u8(m_table.data()+176)}};
        const uint8x16x4_t t3 = {{vld1q_u8(m_table.data()+192), vld1q_u8(m_table.data()+208),
                                vld1q_u8(m_table.data()+224), vld1q_u8(m_table.data()+240)}};
        const uint8x16_t alpha = vreinterpretq_u8_u32(vdupq_n_u32(0xff000000u));
        while (count >= 4) {
            uint8x16_t in = vld1q_u8(pixels);
            uint8x16_t out = vqtbl4q_u8(t0, in);
            out = vqtbx4q_u8(out, t1, vsubq_u8(in, vdupq_n_u8(64)));
            out = vqtbx4q_u8(out, t2, vsubq_u8(in, vdupq_n_u8(128)));
            out = vqtbx4q_u8(out, t3, vsubq_u8(in, vdupq_n_u8(192)));
            vst1q_u8(pixels, vbslq_u8(alpha, in, out));
            pixels += 16; count -= 4;
        }
#endif
        for (size_t i = 0; i < count; i++, pixels += 4)
            for (unsigned c = 0; c < 3; c++) pixels[c] = m_table[pixels[c]];
    }
};

} // namespace vrrt
