#pragma once
#include <cstdint>

// Toucher-style coordinate mapping, mirrored from the `toucher` surface API
// used by imgui-liquid-glass (include/toucher/toucher.h + InputBridge.cpp):
//   - mapCoordinate: normalize a raw device span onto a display span.
//   - apply: rotate (then mirror) a point between source and destination rects.
//   - rotationToTransform: Android Display.getRotation() -> inverse input
//     transform (same mapping as glass_ui::display::inputTransform).

namespace touch {

enum Transform : uint32_t {
    IDENTITY                     = 0,
    ROTATE_90                    = 1,
    ROTATE_180                   = 2,
    ROTATE_270                   = 3,
    MIRROR_HORIZONTAL            = 4,
    MIRROR_HORIZONTAL_ROTATE_90  = 5,
    MIRROR_HORIZONTAL_ROTATE_180 = 6,
    MIRROR_HORIZONTAL_ROTATE_270 = 7,
};

inline float mapCoordinate(float value, uint32_t sourceSpan, uint32_t targetSpan) {
    if (sourceSpan == 0u) return 0.0f;
    return value / static_cast<float>(sourceSpan) * static_cast<float>(targetSpan);
}

// Apply the transform to a point in [0,srcW]x[0,srcH], producing a point in
// [0,dstW]x[0,dstH]. Rotation convention follows Android libui Transform:
// rotate first (clockwise as ROTATE_90), then mirror horizontally.
inline void apply(uint32_t transform,
                  float x, float y,
                  uint32_t srcW, uint32_t srcH,
                  uint32_t dstW, uint32_t dstH,
                  float &outX, float &outY) {
    if (srcW == 0u || srcH == 0u || dstW == 0u || dstH == 0u) {
        outX = 0.0f; outY = 0.0f; return;
    }
    float u = x / static_cast<float>(srcW);
    float v = y / static_cast<float>(srcH);
    const uint32_t rot = transform & 0x3u;
    const bool mirror = (transform & 0x4u) != 0u;
    float ur = u, vr = v;
    switch (rot) {
        case 0u: ur = u;        vr = v;        break;
        case 1u: ur = 1.0f - v; vr = u;        break;  // ROTATE_90  (clockwise)
        case 2u: ur = 1.0f - u; vr = 1.0f - v; break;  // ROTATE_180
        case 3u: ur = v;        vr = 1.0f - u; break;  // ROTATE_270
    }
    if (mirror) ur = 1.0f - ur;
    outX = ur * static_cast<float>(dstW);
    outY = vr * static_cast<float>(dstH);
}

// Android Surface rotation -> inverse input transform. Matches
// glass_ui::display::inputTransform (Rotate90 -> ROTATE_270, etc.).
inline uint32_t rotationToTransform(int32_t rotation) {
    switch (rotation) {
        case 0: return IDENTITY;
        case 1: return ROTATE_270;
        case 2: return ROTATE_180;
        case 3: return ROTATE_90;
        default: return ROTATE_270;
    }
}

}  // namespace touch
