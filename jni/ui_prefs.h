#pragma once

// ============================================================================
// ui_prefs.h —— 界面偏好（纯 UI 状态，不含任何游戏数据）
// ============================================================================
// 只保存「怎么画」：模糊、主题色、圆角、缩放、悬浮球位置。
// 与宿主进程无关，可被任意 Android 应用 + ImGui 覆盖层复用。
//
// 落盘路径可用编译宏覆盖：
//   -DIMGUI_UI_PREFS_PATH='"/data/local/tmp/imgui_ui.txt"'
// 默认先写 /sdcard，失败退到 /data/local/tmp。
// ============================================================================

#include <cstdio>
#include <cstdint>

#ifndef IMGUI_UI_PREFS_PATH
#define IMGUI_UI_PREFS_PATH "/sdcard/imgui_ui.txt"
#endif
#ifndef IMGUI_UI_PREFS_PATH_FALLBACK
#define IMGUI_UI_PREFS_PATH_FALLBACK "/data/local/tmp/imgui_ui.txt"
#endif

namespace ui {

struct Prefs {
    // 背景模糊
    bool  blurOn        = true;
    float blurAmount    = 1.00f;
    float blurThreshold = 0.00f;

    // 主题
    float accentHue  = 0.055f;   // 暖橙
    float accentSat  = 0.80f;
    bool  darkTheme  = true;
    bool  forceBlack = false;

    // 面板
    float bgAlpha   = 0.90f;
    float uiScale   = 1.00f;
    float scrollW   = 18.0f;
    float imgRound  = 0.26f;
    float menuRound = 22.0f;
    float tabRound  = 9.0f;
    float menuW     = 0.0f;      // 0 = 自动按 4:3 计算
    float menuH     = 0.0f;
    float menuX     = -1.0f;     // <0 = 居中
    float menuY     = -1.0f;

    // 悬浮球
    float ballScale = 1.00f;
    float ballAlpha = 1.00f;
    float ballX     = 0.885f;
    float ballY     = 0.500f;
};

inline Prefs prefs;

// 主题色预设（hue, sat）
struct Swatch { const char* name; float h; float s; };
inline const Swatch kSwatches[6] = {
    { "暖　橙", 0.055f, 0.80f },
    { "青　蓝", 0.545f, 0.62f },
    { "翠　绿", 0.330f, 0.65f },
    { "樱　粉", 0.940f, 0.55f },
    { "紫罗兰", 0.750f, 0.55f },
    { "纯　黑", 0.000f, 0.000f },   // sat=0 → 纯黑白字
};

inline const char* const kPath        = IMGUI_UI_PREFS_PATH;
inline const char* const kPathAlt     = IMGUI_UI_PREFS_PATH_FALLBACK;

inline bool save() {
    const char* paths[2] = { kPath, kPathAlt };
    for (int i = 0; i < 2; ++i) {
        FILE* f = fopen(paths[i], "w");
        if (!f) continue;
        fprintf(f,
            "blurOn=%d\nblurAmount=%.6g\nblurThreshold=%.6g\n"
            "darkTheme=%d\nforceBlack=%d\naccentHue=%.6g\naccentSat=%.6g\n"
            "bgAlpha=%.6g\nuiScale=%.6g\nscrollW=%.6g\nimgRound=%.6g\n"
            "menuW=%.6g\nmenuH=%.6g\nmenuX=%.6g\nmenuY=%.6g\n"
            "ballScale=%.6g\nballAlpha=%.6g\nballX=%.6g\nballY=%.6g\n"
            "menuRound=%.6g\ntabRound=%.6g\n",
            prefs.blurOn ? 1 : 0, prefs.blurAmount, prefs.blurThreshold,
            prefs.darkTheme ? 1 : 0, prefs.forceBlack ? 1 : 0,
            prefs.accentHue, prefs.accentSat, prefs.bgAlpha, prefs.uiScale,
            prefs.scrollW, prefs.imgRound, prefs.menuW, prefs.menuH,
            prefs.menuX, prefs.menuY, prefs.ballScale, prefs.ballAlpha,
            prefs.ballX, prefs.ballY, prefs.menuRound, prefs.tabRound);
        fclose(f);
        return true;
    }
    return false;
}

inline bool load() {
    const char* paths[2] = { kPath, kPathAlt };
    for (int i = 0; i < 2; ++i) {
        FILE* f = fopen(paths[i], "r");
        if (!f) continue;
        Prefs p;
        int blur = 1, dark = 1, black = 0;
        const int got = fscanf(f,
            "blurOn=%d\nblurAmount=%f\nblurThreshold=%f\n"
            "darkTheme=%d\nforceBlack=%d\naccentHue=%f\naccentSat=%f\n"
            "bgAlpha=%f\nuiScale=%f\nscrollW=%f\nimgRound=%f\n"
            "menuW=%f\nmenuH=%f\nmenuX=%f\nmenuY=%f\n"
            "ballScale=%f\nballAlpha=%f\nballX=%f\nballY=%f\n"
            "menuRound=%f\ntabRound=%f",
            &blur, &p.blurAmount, &p.blurThreshold,
            &dark, &black, &p.accentHue, &p.accentSat, &p.bgAlpha, &p.uiScale,
            &p.scrollW, &p.imgRound, &p.menuW, &p.menuH, &p.menuX, &p.menuY,
            &p.ballScale, &p.ballAlpha, &p.ballX, &p.ballY,
            &p.menuRound, &p.tabRound);
        fclose(f);
        if (got < 19) continue;
        p.blurOn     = (blur  != 0);
        p.darkTheme  = (dark  != 0);
        p.forceBlack = (black != 0);
        prefs = p;
        return true;
    }
    return false;
}

inline void reset() { prefs = Prefs(); }

// ---------------------------------------------------------------------------
// HSV -> ImU32（IM_COL32 布局：R 在低字节）
// ---------------------------------------------------------------------------
inline unsigned int hsv(float h, float s, float v, float a) {
    h = h - (float)(int)h;
    if (h < 0.0f) h += 1.0f;

    const float hh = h * 6.0f;
    const int   i  = (int)hh;
    const float f  = hh - (float)i;
    const float p  = v * (1.0f - s);
    const float q  = v * (1.0f - s * f);
    const float t  = v * (1.0f - s * (1.0f - f));

    float r = v, g = v, b = v;
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }

    const float ca = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    const float cr = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    const float cg = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
    const float cb = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);

    const unsigned int A = (unsigned int)(ca * 255.0f + 0.5f);
    const unsigned int R = (unsigned int)(cr * 255.0f + 0.5f);
    const unsigned int G = (unsigned int)(cg * 255.0f + 0.5f);
    const unsigned int B = (unsigned int)(cb * 255.0f + 0.5f);
    return (A << 24) | (B << 16) | (G << 8) | R;
}

}  // namespace ui
