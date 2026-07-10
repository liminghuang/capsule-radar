#pragma once

#include <math.h>
#include <stdint.h>

// One horizontal row of a circular annulus normally has two short spans. Keeping
// those spans separate is the key to avoiding the old full bounding-box refresh.
struct RippleRowSpans {
    int16_t leftStart = 0, leftEnd = -1;
    int16_t rightStart = 0, rightEnd = -1;
    bool valid = false;
};

// Keep the outer edge visible on a black AMOLED panel.  A value near 10% is
// mathematically present but visually lost once the halo is alpha-composited.
inline uint8_t rippleOpacity(float phase, uint8_t coreOpacity, uint8_t edgeOpacity) {
    if (phase < 0.0f) phase = 0.0f;
    if (phase > 1.0f) phase = 1.0f;
    return (uint8_t)((float)edgeOpacity +
                     ((float)coreOpacity - (float)edgeOpacity) * (1.0f - phase));
}

inline RippleRowSpans rippleRowSpans(int16_t cx, int16_t cy, float radius,
                                     float width, int16_t y,
                                     int16_t minX, int16_t maxX) {
    RippleRowSpans out;
    const float dy = fabsf((float)y - (float)cy);
    const float outerR = radius + width * 0.5f;
    const float innerR = radius - width * 0.5f;
    if (outerR <= 0.0f || dy > outerR) return out;

    const int16_t outerDx = (int16_t)floorf(sqrtf(outerR * outerR - dy * dy));
    const int16_t innerDx = (innerR > 0.0f && dy < innerR)
        ? (int16_t)ceilf(sqrtf(innerR * innerR - dy * dy)) : 0;
    out.leftStart  = (int16_t)(cx - outerDx);
    out.leftEnd    = (int16_t)(cx - innerDx);
    out.rightStart = (int16_t)(cx + innerDx);
    out.rightEnd   = (int16_t)(cx + outerDx);
    if (out.leftStart < minX) out.leftStart = minX;
    if (out.rightEnd > maxX) out.rightEnd = maxX;
    if (out.leftEnd > maxX) out.leftEnd = maxX;
    if (out.rightStart < minX) out.rightStart = minX;
    out.valid = out.leftStart <= out.leftEnd || out.rightStart <= out.rightEnd;
    return out;
}
