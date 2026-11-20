#pragma once
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "touch_transform.h"

// ============================================================================
// 触摸坐标：运行时模式切换 + 四点自动标定 (khimgui)
// ============================================================================
//
// 【为什么之前 7 轮都没修好】
//
// 前 7 轮的每一次修改都是先在源码里"选定一种映射假设"，然后重编译 ->
// 重新注入 -> 重新截图 -> 发现还偏。这些假设分别是：
//
//   ① getRawX/getRawY * 比例
//   ② getX/getY（窗口本地）* window/surface
//   ③ EGL surface 坐标 1:1，DisplaySize=surface
//   ④ window 逻辑尺寸 + DisplayFramebufferScale
//   ⑤ toucher 式旋转 + 归一化
//
// 假设之间互斥，而且**没有任何一次拿到过量化的现场数据**，
// 所以每轮只能修对三分之一，然后在下一轮被推翻。
//
// 更关键的是一个此前从未被检查过的事实：
//   · Display.getRotation() 在横屏游戏里通常返回 1 或 3
//   · 而 Display.getRealSize() 返回的是**当前旋转下**的尺寸（已是横向）
//   · AMotionEvent_getRawX/Y 也是**当前旋转下**的屏幕坐标
//   => 在这台机器上，raw 坐标其实已经是"横向并且和 surface 同向"的，
//      再套一次 rotationToTransform 就等于**二次旋转**。
//   反过来，如果这台机器上 Unity 的 surface 小于屏幕（由 SurfaceFlinger
//   放大上屏），那正确做法是"缩放到 surface"，根本不需要旋转。
//   这两种情况在源码里长得一模一样，靠猜永远分不清。
//
// 【这一层的做法】
//
// 不再猜。把"映射"拆成一个可在真机上秒切的开关，加一个能把任意仿射
// 误差吃掉的四点标定：
//
//   最终坐标 = 仿射( 模式映射( 触摸事件 ) )
//
//   模式 0：窗口本地 1:1              （getX/getY 直接当 surface 像素）
//   模式 1：屏幕原始 1:1              （getRawX/getRawY 直接当 surface 像素）
//   模式 2：窗口本地 → 缩放至 surface
//   模式 3：屏幕原始 → 逆旋转 → 归一化到 surface（toucher 原逻辑）
//
//   仿射：四个参数组 (a,b,c,d,tx,ty)，默认恒等，enabled=false，
//         即默认行为与"未经标定"完全一致，不改变任何现状。
//
// 【四点标定为什么能一次收敛】
//
// 屏幕上画 4 个编号靶心，手指依次点过去。把"管线输出坐标"和"靶心的
// 逻辑坐标"做最小二乘拟合，解出 2x2 + 平移。不论真实成因是缩放、平移、
// 旋转、镜像，还是它们的任意组合，只要映射是线性的，这个仿射矩阵就能
// 精确抵消。拟合完成后会报出最大残差：
//   · 残差 < 3px  → 映射是纯仿射，已经彻底对齐
//   · 残差很大    → 存在非线性（分块缩放/畸变），此时也是硬结论，
//                   不必再猜是不是自己写错了。

namespace touch {
namespace calib {

// ---- 映射模式 ----
enum Mode : int {
    MODE_RAW_SCALE   = 0,   // 屏幕原始 → 缩放到 surface（★ 最可能正确）
    MODE_RAW_1TO1    = 1,   // 屏幕原始 1:1
    MODE_WINDOW_1TO1 = 2,   // 窗口本地 1:1
    MODE_WINDOW_SCALE= 3,   // 窗口本地 → 缩放至 surface
    MODE_RAW_ROTATE  = 4,   // 屏幕原始 → 逆旋转 → 归一化（toucher 原逻辑）
    MODE_COUNT       = 5,
};
inline int mode = MODE_RAW_SCALE;

static inline const char* modeName(int m) {
    switch (m) {
        case MODE_RAW_SCALE:    return "屏幕原始 → 缩放至 surface";
        case MODE_RAW_1TO1:     return "屏幕原始 1:1";
        case MODE_WINDOW_1TO1:  return "窗口本地 1:1";
        case MODE_WINDOW_SCALE: return "窗口本地 → 缩放至 surface";
        case MODE_RAW_ROTATE:   return "屏幕原始 → 逆旋转 → 归一化";
        default:                return "?";
    }
}

// ---- 每帧由 hook.cpp 填写的现场 ----
inline float    surfaceW = 0.0f, surfaceH = 0.0f;
inline float    displayW = 0.0f, displayH = 0.0f;   // getRealSize（当前旋转下）
inline float    windowW  = 0.0f, windowH  = 0.0f;   // ANativeWindow_getWidth/Height
inline int32_t  rotation = -1;
inline uint32_t xform    = 0;                        // rotationToTransform(rotation)

// ---- 仿射：final = A * mapped + t ----
inline float a  = 1.0f, b  = 0.0f;   // final.x = a*mx + b*my + tx
inline float c  = 0.0f, d  = 1.0f;   // final.y = c*mx + d*my + ty
inline float tx = 0.0f, ty = 0.0f;
inline bool  enabled  = false;
inline int   fitMode  = -1;              // 拟合时用的映射模式

// ---- 采样（供屏幕检视器显示） ----
inline bool  hasPending = false;                 // 有一次「按下」待标定消费
inline float pendX = 0.0f, pendY = 0.0f;         // 该次按下的「映射后」坐标

inline bool  hasSample = false;
inline float lastWX = 0.0f, lastWY = 0.0f;       // getX/getY
inline float lastRX = 0.0f, lastRY = 0.0f;       // getRawX/getRawY
inline float lastMX = 0.0f, lastMY = 0.0f;       // 模式映射之后
inline float lastFX = 0.0f, lastFY = 0.0f;       // 仿射之后（ImGui 实际用值）

// ---- 四点标定状态 ----
inline bool  learning = false;
inline int   stage    = 0;
inline bool  have[4]  = {false, false, false, false};
inline float capX[4]  = {0, 0, 0, 0};            // 采到的「映射后」坐标
inline float capY[4]  = {0, 0, 0, 0};
inline float tgtX[4]  = {0, 0, 0, 0};            // 对应靶心的逻辑坐标
inline float tgtY[4]  = {0, 0, 0, 0};
inline float fitErr   = -1.0f;

inline char status[192] = "未标定（恒等映射）";

// ---------------------------------------------------------------------------
// 模式映射：把一笔触摸事件换算到 ImGui 的坐标空间（surface 像素）
// ---------------------------------------------------------------------------
inline void mapPoint(float wx, float wy, float rx, float ry,
                     float& outX, float& outY) {
    outX = wx; outY = wy;
    switch (mode) {
        case MODE_WINDOW_1TO1:
            outX = wx; outY = wy;
            break;

        case MODE_RAW_1TO1:
            outX = rx; outY = ry;
            break;

        case MODE_WINDOW_SCALE:
            if (displayW > 1.0f && displayH > 1.0f && surfaceW > 1.0f && surfaceH > 1.0f) {
                outX = wx * (surfaceW / displayW);
                outY = wy * (surfaceH / displayH);
            } else {
                outX = wx; outY = wy;
            }
            break;

        case MODE_RAW_SCALE:
            // ★ 主力模式。横向 Unity 游戏 surface 常小于屏幕（SurfaceFlinger
            //   放大上屏），raw 已是当前旋转下的屏幕坐标，不需要任何旋转，
            //   只做一次 display -> surface 的等比缩放。
            if (displayW > 1.0f && displayH > 1.0f && surfaceW > 1.0f && surfaceH > 1.0f) {
                outX = rx * (surfaceW / displayW);
                outY = ry * (surfaceH / displayH);
            } else if (surfaceW > 1.0f && surfaceH > 1.0f && windowW > 1.0f && windowH > 1.0f) {
                outX = rx * (surfaceW / windowW);
                outY = ry * (surfaceH / windowH);
            } else {
                outX = rx; outY = ry;
            }
            break;

        case MODE_RAW_ROTATE:
        default:
            if (displayW > 1.0f && displayH > 1.0f && surfaceW > 1.0f && surfaceH > 1.0f) {
                touch::apply(xform, rx, ry,
                             (uint32_t)displayW, (uint32_t)displayH,
                             (uint32_t)surfaceW, (uint32_t)surfaceH,
                             outX, outY);
            } else {
                outX = rx; outY = ry;
            }
            break;
    }
}

// 输入线程调用：记录样本，并在「按下」时挂一个待标定样本
inline void note(float wx, float wy, float rx, float ry, bool down) {
    hasSample = true;
    lastWX = wx; lastWY = wy;
    lastRX = rx; lastRY = ry;
    mapPoint(wx, wy, rx, ry, lastMX, lastMY);
    if (down) {
        hasPending = true;
        pendX = lastMX;
        pendY = lastMY;
    }
}

// 渲染线程调用：套用仿射，得到 ImGui 实际使用值
inline void applyPoint(float& x, float& y) {
    if (enabled) {
        const float mx = x, my = y;
        x = a * mx + b * my + tx;
        y = c * mx + d * my + ty;
    }
    lastFX = x; lastFY = y;
}

// 合并：从一笔事件直接算出最终坐标
inline void resolve(float wx, float wy, float rx, float ry,
                    float& outX, float& outY) {
    mapPoint(wx, wy, rx, ry, outX, outY);
    applyPoint(outX, outY);
    lastFX = outX; lastFY = outY;
}

// 第 i 个靶心的逻辑坐标（避开四角系统手势区，也避开面板）
inline void target(int i, float W, float H, float& x, float& y) {
    const float fx = (i == 0 || i == 2) ? 0.12f : 0.88f;
    const float fy = (i < 2)           ? 0.12f : 0.88f;
    x = fx * W;
    y = fy * H;
}

// ---------------------------------------------------------------------------
// 最小二乘：L ≈ p*Cx + q*Cy + r
// ---------------------------------------------------------------------------
inline bool solveAxis(const float* Cx, const float* Cy, const float* L,
                      float& p, float& q, float& r) {
    double Sxx = 0, Sxy = 0, Sx = 0, Syy = 0, Sy = 0, N = 0;
    double SLx = 0, SLy = 0, SL = 0;
    for (int i = 0; i < 4; ++i) {
        Sxx += (double)Cx[i] * Cx[i];
        Sxy += (double)Cx[i] * Cy[i];
        Sx  += (double)Cx[i];
        Syy += (double)Cy[i] * Cy[i];
        Sy  += (double)Cy[i];
        N   += 1.0;
        SLx += (double)Cx[i] * L[i];
        SLy += (double)Cy[i] * L[i];
        SL  += (double)L[i];
    }
    double M[3][3] = {{Sxx, Sxy, Sx}, {Sxy, Syy, Sy}, {Sx, Sy, N}};
    double V[3]    = {SLx, SLy, SL};

    for (int k = 0; k < 3; ++k) {
        int piv = k;
        for (int i = k + 1; i < 3; ++i) {
            const double ai = M[i][k]   < 0 ? -M[i][k]   : M[i][k];
            const double ap = M[piv][k] < 0 ? -M[piv][k] : M[piv][k];
            if (ai > ap) piv = i;
        }
        const double diag = M[piv][k] < 0 ? -M[piv][k] : M[piv][k];
        if (diag < 1e-9) return false;                 // 退化（点共线）
        if (piv != k) {
            for (int j = 0; j < 3; ++j) { double t = M[k][j]; M[k][j] = M[piv][j]; M[piv][j] = t; }
            double t = V[k]; V[k] = V[piv]; V[piv] = t;
        }
        for (int i = k + 1; i < 3; ++i) {
            const double f = M[i][k] / M[k][k];
            for (int j = k; j < 3; ++j) M[i][j] -= f * M[k][j];
            V[i] -= f * V[k];
        }
    }
    double s[3] = {0, 0, 0};
    for (int i = 2; i >= 0; --i) {
        double acc = V[i];
        for (int j = i + 1; j < 3; ++j) acc -= M[i][j] * s[j];
        s[i] = acc / M[i][i];
    }
    p = (float)s[0]; q = (float)s[1]; r = (float)s[2];
    return true;
}

inline bool fit() {
    for (int i = 0; i < 4; ++i) if (!have[i]) return false;

    // 直接拟合「采样 -> 靶心」的仿射：
    //   tgtX ≈ a*capX + b*capY + tx
    //   tgtY ≈ c*capX + d*capY + ty
    // solveAxis 解的就是这组 (p,q,r) = (a,b,tx) / (c,d,ty)，无需再求逆。
    float A, B, TX, C, D, TY;
    if (!solveAxis(capX, capY, tgtX, A, B, TX)) return false;
    if (!solveAxis(capX, capY, tgtY, C, D, TY)) return false;

    const float det = A * D - B * C;
    if (det > -1e-9f && det < 1e-9f) return false;   // 退化（共线 / 镜像塌陷）

    a = A; b = B; tx = TX;
    c = C; d = D; ty = TY;

    float e = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float px = a * capX[i] + b * capY[i] + tx;
        const float py = c * capX[i] + d * capY[i] + ty;
        const float ex = px - tgtX[i] < 0 ? tgtX[i] - px : px - tgtX[i];
        const float ey = py - tgtY[i] < 0 ? tgtY[i] - py : py - tgtY[i];
        if (ex > e) e = ex;
        if (ey > e) e = ey;
    }
    fitErr  = e;
    fitMode = mode;
    enabled = true;
    snprintf(status, sizeof(status), "标定完成（mode=%d），最大残差 %.1f px", mode, e);
    return true;
}

inline void reset() {
    a = 1.0f; b = 0.0f; c = 0.0f; d = 1.0f; tx = 0.0f; ty = 0.0f;
    enabled  = false;
    learning = false;
    stage    = 0;
    fitMode  = -1;
    for (int i = 0; i < 4; ++i) have[i] = false;
    fitErr = -1.0f;
    snprintf(status, sizeof(status), "已清除（恒等映射）");
}

// ---------------------------------------------------------------------------
// 持久化：外部存储优先，失败退到 /data/local/tmp
// ---------------------------------------------------------------------------
#ifndef IMGUI_UI_CALIB_PATH
#define IMGUI_UI_CALIB_PATH "/sdcard/imgui_ui_calib.txt"
#endif
#ifndef IMGUI_UI_CALIB_PATH_FALLBACK
#define IMGUI_UI_CALIB_PATH_FALLBACK "/data/local/tmp/imgui_ui_calib.txt"
#endif
inline const char* kSavePaths[2] = {
    IMGUI_UI_CALIB_PATH,
    IMGUI_UI_CALIB_PATH_FALLBACK,
};

inline bool save() {
    for (int k = 0; k < 2; ++k) {
        FILE* f = fopen(kSavePaths[k], "w");
        if (!f) continue;
        fprintf(f,
            "mode=%d\nfitmode=%d\na=%.9g\nb=%.9g\nc=%.9g\nd=%.9g\ntx=%.9g\nty=%.9g\nenabled=%d\n",
            mode, fitMode, a, b, c, d, tx, ty, enabled ? 1 : 0);
        fclose(f);
        snprintf(status, sizeof(status), "已保存 → %s", kSavePaths[k]);
        return true;
    }
    snprintf(status, sizeof(status), "保存失败：两处路径均不可写");
    return false;
}

inline bool load() {
    for (int k = 0; k < 2; ++k) {
        FILE* f = fopen(kSavePaths[k], "r");
        if (!f) continue;
        int m = MODE_WINDOW_1TO1, fm = -1, en = 0;
        float na = 1, nb = 0, nc = 0, nd = 1, ntx = 0, nty = 0;
        const int got = fscanf(f,
            "mode=%d\nfitmode=%d\na=%f\nb=%f\nc=%f\nd=%f\ntx=%f\nty=%f\nenabled=%d",
            &m, &fm, &na, &nb, &nc, &nd, &ntx, &nty, &en);
        fclose(f);
        if (got < 9) continue;
        if (m  >= 0 && m  < MODE_COUNT) mode = m;
        if (fm >= 0 && fm < MODE_COUNT) fitMode = fm;
        a = na; b = nb; c = nc; d = nd; tx = ntx; ty = nty;
        enabled = (en != 0);
        snprintf(status, sizeof(status), "已读取 ← %s", kSavePaths[k]);
        return true;
    }
    return false;
}

}  // namespace calib
}  // namespace touch
