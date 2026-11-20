// ============================================================================
// ui_menu.cpp —— 覆盖层界面本体（主题 / 控件 / 分页 / 悬浮球 / 交互）
// ============================================================================
// 这里保留的是原项目里"怎么画"的全部东西：
//   · 主题构建（深色 / 纯黑 / 浅色 + 主题色反相规则）
//   · 自绘控件：iOS 风开关（弹簧按压 + 贝塞尔过冲）、滑条、分段标题、按钮
//   · 分区翻页（竖向滚动切换 + 双页同时插值）
//   · 悬浮球拖拽开合、图片长按拖动、右下角自由缩放、侧滑条
//   · 面板模糊（抓菜单矩形做高斯，圆角保形）
//   · 诗句标题（开一次菜单换一句，不逐帧跳）
//
// 与功能相关的部分已经摘掉：
//   原来的 实用 / 娱乐 / 素材 / 调试 四个分区都是游戏功能开关，
//   现在换成 概览 / 控件 / 布局 / 主题 四个纯 UI 演示分区。
//   这里不存在任何游戏内存读写、方法调用、网络验证。
// ============================================================================

#include <imgui.h>

#include <EGL/egl.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "ball_texture.h"
#include "flcy_log.h"
#include "gl_blur.h"
#include "ui_layer.h"
#include "ui_prefs.h"

#define FLCY_LOG(...) flcylog::Write("ImGuiUI", __VA_ARGS__)

namespace {

// ============================================================================
// 工具
// ============================================================================
inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

float Approach(float cur, float target, float rate, float dt) {
    if (dt <= 0.0f) return cur;
    const float k = 1.0f - expf(-rate * dt);
    return cur + (target - cur) * k;
}

float EaseOutCubic(float t) {
    t = Clamp01(t); const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// 三次贝塞尔缓动（等价 CSS cubic-bezier）
// 之前是"线性推进 + 一次性 ease-out"，起手处一阶导数有尖角，观感上就是
// 没有曲线的关键帧。这里换成真正的贝塞尔：速度曲线光滑、起收切线连续。
float BezierAxis(float t, float a1, float a2) {
    const float u = Clamp01(t);
    float s = u;
    for (int i = 0; i < 6; ++i) {
        const float om = 1.0f - s;
        const float x  = 3.0f * om * om * s * a1 + 3.0f * om * s * s * a2 + s * s * s;
        const float dx = 3.0f * om * om * a1
                       + 6.0f * om * s * (a2 - a1)
                       + 3.0f * s * s * (1.0f - a2);
        if (fabsf(dx) < 1e-6f) break;
        s -= (x - u) / dx;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
    }
    const float om = 1.0f - s;
    return 3.0f * om * s * s + s * s * s;
}

// 控制点按"速度曲线最平滑"反算：
//   max|d²| 越小越没有关键帧感；peak_v 落在 t~0.29~0.36，中段最快两端都慢。
float EaseDecel(float t)  { return BezierAxis(t, 0.36f, 0.40f); }  // 开
float EaseAccel(float t)  { return BezierAxis(t, 0.32f, 0.30f); }  // 关
float EaseSmooth(float t) { return BezierAxis(t, 0.30f, 0.34f); }  // 翻页

// ============================================================================
// 全局状态（全部是本模块私有的 UI 状态）
// ============================================================================
float g_lastDt = 0.016f;

bool  g_menuOpen = false;
float g_openT    = 0.0f;      // 悬浮球淡出
float g_ballT    = 1.0f;      // 悬浮球存在度
float g_animT    = 0.0f;      // 菜单开合进度 0..1
const float kOpenSec  = 0.34f;
const float kCloseSec = 0.26f;

bool  g_dragging = false;
float g_dragDX = 0.0f, g_dragDY = 0.0f;
bool  g_ballWasDown = false;
bool  g_ballMoved = false;

float g_menuScroll = 0.0f;    // 内容区滚动偏移

int   g_tab = 0;
int   g_prevTab = -1;         // 正在滚出的旧分区
int   g_tabDir = 1;           // +1 向右滚，-1 向左滚
float g_pageX = 1.0f;
int   g_dragSlider = -1;      // 正在拖的滑条 id

// 开关动画进度：按 slot 索引
const int kSwAnimN = 128;
float g_swAnim[kSwAnimN] = { 0 };

inline int SwSlot(int slot, int fallback) {
    if (slot < 0) return fallback;
    return slot % kSwAnimN;
}

// 左上图片拖动
bool  g_imgHeld = false;
float g_imgHoldT = 0.0f;
bool  g_imgDragging = false;
float g_imgDragDX = 0.0f, g_imgDragDY = 0.0f;
bool  g_imgWasDown = false;
bool  g_imgMoved = false;
const float kHoldSec = 0.30f;

// 右下角缩放
bool  g_resizing = false;
float g_resizeStartX = 0.0f, g_resizeStartY = 0.0f;
float g_resizeStartW = 0.0f, g_resizeStartH = 0.0f;

// ============================================================================
// 几何
// ============================================================================
float BallBaseSize(float W, float H) {
    const float side = (W < H ? W : H);
    float d = side * 0.088f;
    if (d < 56.0f)  d = 56.0f;
    if (d > 132.0f) d = 132.0f;
    return d;
}
float BallSize(float W, float H) { return BallBaseSize(W, H) * ui::prefs.ballScale; }

void BallCenter(float W, float H, float& cx, float& cy) {
    cx = ui::prefs.ballX * W;
    cy = ui::prefs.ballY * H;
    const float rad = BallSize(W, H) * 0.5f * 1.2f + 8.0f;
    if (cx < rad)     cx = rad;
    if (cx > W - rad) cx = W - rad;
    if (cy < rad)     cy = rad;
    if (cy > H - rad) cy = H - rad;
}

void MenuRect(float W, float H, float& x, float& y, float& w, float& h) {
    ui::Prefs& p = ui::prefs;
    if (p.menuW <= 1.0f || p.menuH <= 1.0f) {
        float mw = W * 0.78f;
        float mh = mw * 3.0f / 4.0f;
        if (mh > H * 0.86f) { mh = H * 0.86f; mw = mh * 4.0f / 3.0f; }
        if (mw < 420.0f) { mw = 420.0f; mh = mw * 3.0f / 4.0f; }
        w = mw; h = mh;
    } else {
        w = p.menuW; h = p.menuH;
    }
    if (w > W - 12.0f) w = W - 12.0f;
    if (h > H - 12.0f) h = H - 12.0f;
    if (w < 300.0f) w = 300.0f;
    if (h < 220.0f) h = 220.0f;

    x = (p.menuX < 0.0f) ? (W - w) * 0.5f : p.menuX;
    y = (p.menuY < 0.0f) ? (H - h) * 0.5f : p.menuY;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x + w > W) x = W - w;
    if (y + h > H) y = H - h;
}

// ============================================================================
// 主题
// ============================================================================
struct Theme {
    ImU32 text, textDim, panel, panelSolid, sidebar, card, cardHover, sep;
    ImU32 accent, accentDim, swOff, swOffHover, knob;
    ImU32 swOn;          // 开关开启色（固定绿，不跟主题）
};
Theme g_theme;

bool UseDark() { return ui::prefs.forceBlack || ui::prefs.darkTheme; }
bool UsePureBlack() { return ui::prefs.forceBlack; }

void BuildTheme() {
    ui::Prefs& p = ui::prefs;
    const bool dark = UseDark();

    // 主题色选「纯黑」(sat==0) 时，深色底上黑色 accent 看不见，
    // 反相成近白 —— 黑底白线。
    if (p.accentSat < 0.01f) {
        g_theme.accent = dark ? IM_COL32(240, 240, 240, 255)
                              : IM_COL32(20, 20, 20, 255);
        g_theme.accentDim = dark ? IM_COL32(170, 170, 170, 255)
                                 : IM_COL32(90, 90, 90, 255);
    } else {
        g_theme.accent    = ui::hsv(p.accentHue, p.accentSat, dark ? 0.95f : 0.88f, 1.0f);
        g_theme.accentDim = ui::hsv(p.accentHue, p.accentSat, dark ? 0.62f : 0.72f, 1.0f);
    }

    // 开关开启色固定为绿（iOS 系统绿）
    g_theme.swOn = IM_COL32(52, 199, 89, 255);

    const bool black = UsePureBlack();

    if (dark) {
        const int pa = p.blurOn ? 92 : (int)(255 * p.bgAlpha);
        if (black) {
            g_theme.panel = IM_COL32(0, 0, 0, p.blurOn ? pa : 255);
        } else {
            g_theme.panel = IM_COL32(22, 22, 26, p.blurOn ? pa : 255);
        }
        g_theme.panelSolid = black ? IM_COL32(0, 0, 0, 255)
                                   : IM_COL32(22, 22, 26, 255);
        g_theme.sidebar    = IM_COL32(0, 0, 0, 0);
        g_theme.card       = black ? IM_COL32(255, 255, 255, 18)
                                   : IM_COL32(255, 255, 255, 16);
        g_theme.cardHover  = IM_COL32(255, 255, 255, 30);
        g_theme.sep        = IM_COL32(255, 255, 255, 28);
        g_theme.text       = IM_COL32(255, 255, 255, 255);
        g_theme.textDim    = IM_COL32(255, 255, 255, 165);
        g_theme.swOff      = IM_COL32(58, 58, 62, 255);
        g_theme.swOffHover = IM_COL32(72, 72, 78, 255);
        g_theme.knob       = IM_COL32(255, 255, 255, 255);
    } else {
        g_theme.panel      = IM_COL32(250, 249, 247, (int)(255 * p.bgAlpha));
        g_theme.panelSolid = g_theme.panel;
        g_theme.sidebar    = IM_COL32(238, 236, 232, (int)(255 * p.bgAlpha));
        g_theme.card       = IM_COL32(0, 0, 0, 14);
        g_theme.cardHover  = IM_COL32(0, 0, 0, 26);
        g_theme.sep        = IM_COL32(0, 0, 0, 30);
        g_theme.text       = IM_COL32(0, 0, 0, 255);
        g_theme.textDim    = IM_COL32(0, 0, 0, 160);
        g_theme.swOff      = IM_COL32(190, 190, 194, 255);
        g_theme.swOffHover = IM_COL32(206, 206, 210, 255);
        g_theme.knob       = IM_COL32(255, 255, 255, 255);
    }
}

// ============================================================================
// 悬浮球
// ============================================================================
void DrawBall(float W, float H) {
    if (g_ballT <= 0.002f) return;
    float cx, cy;
    BallCenter(W, H, cx, cy);
    const float baseR = BallSize(W, H) * 0.5f;
    const float r = baseR * Clamp01(g_ballT) * (g_dragging ? 1.05f : 1.0f);
    if (r <= 0.5f) return;

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float alpha = Clamp01(ui::prefs.ballAlpha * Clamp01(g_ballT * 2.2f));
    if (alpha <= 0.004f) return;

    const float rBlack = r + (r * 0.075f) + 2.0f;
    const float rWhite = rBlack + (r * 0.105f) + 2.5f;

    fg->AddCircleFilled(ImVec2(cx + 1.5f, cy + 3.0f), rWhite,
        IM_COL32(0, 0, 0, (int)(85 * alpha)), 72);
    fg->AddCircleFilled(ImVec2(cx, cy), rWhite,
        IM_COL32(255, 255, 255, (int)(240 * alpha)), 72);
    fg->AddCircleFilled(ImVec2(cx, cy), rBlack,
        IM_COL32(6, 6, 8, (int)(248 * alpha)), 72);

    const uint32_t tex = ui_layer::BallTexture();
    if (tex) {
        fg->AddImageRounded((ImTextureID)tex,
                            ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32(255, 255, 255, (int)(255 * alpha)), r, 0);
    } else {
        fg->AddCircleFilled(ImVec2(cx, cy), r,
            IM_COL32(58, 70, 92, (int)(255 * alpha)), 72);
    }
    fg->AddCircleFilled(ImVec2(cx - r * 0.28f, cy - r * 0.34f), r * 0.28f,
        IM_COL32(255, 255, 255, (int)(26 * alpha)), 48);
}

// ============================================================================
// 菜单模糊
// ============================================================================
// 只抓菜单矩形那一片做高斯。
// 关键：用 AddImageRounded 画，圆角才能保持；用 AddImage 会变成方角矩形。
// 模糊是"稳定的底"，不参与开合动画，避免背景跟着抖。
void DrawMenuBlur(float H, float mx, float my, float mw, float mh, float round,
                  float animT, float alpha) {
    if (!ui::prefs.blurOn) return;
    if (animT <= 0.02f || alpha <= 0.02f) return;

    blur::g_spread    = ui::prefs.blurAmount;
    blur::g_threshold = ui::prefs.blurThreshold;

    const GLuint tex = blur::runRegion((int)mx, (int)my, (int)mw, (int)mh, (int)H);
    if (!tex) return;

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    // UV 上下翻转：glCopyTexSubImage2D 第 0 行 = 矩形底边
    bg->AddImageRounded((ImTextureID)tex,
                        ImVec2(mx, my), ImVec2(mx + mw, my + mh),
                        ImVec2(0, 1), ImVec2(1, 0),
                        IM_COL32(255, 255, 255, (int)(255.0f * Clamp01(alpha))),
                        round, 0);
}

// ============================================================================
// 内容裁剪
// ============================================================================
// 面板是圆角的，功能块/内容区都是直角矩形。只要它们贴近面板底边或右边，
// 直角外角就会露出圆角轮廓之外 —— 动画中面板被缩放时更明显。
// 兜底：给所有内容绘制加一层"面板内缩后的圆角裁剪"。
void PushPanelClip(ImDrawList* dl, const ImVec2& wp, const ImVec2& ws,
                   float round, float pad) {
    const float inset = 2.0f;
    (void)round; (void)pad;
    dl->PushClipRect(ImVec2(wp.x + inset, wp.y + inset),
                     ImVec2(wp.x + ws.x - inset, wp.y + ws.y - inset), true);
}

// ============================================================================
// 手绘 UI 原语
// ============================================================================
// 全部直接写进主窗口的 DrawList，不用 BeginChild。
// 用 BeginChild 的话子窗口有独立 DrawList：顶点缩放不跟随、
// 裁剪/位置与主窗口不一致，内容会被裁掉。
struct Ui {
    ImDrawList* dl = nullptr;
    float x0 = 0, y0 = 0;     // 内容区左上（屏幕坐标）
    float w = 0;              // 内容可用宽度
    float y = 0;              // 当前光标（未减 scroll）
    float sy = 0;             // 滚动偏移
    float s = 1.0f;           // uiScale
    float th = 0;             // 一行文字高度
};

// ============================================================================
// 按压缩放反馈 —— 仿 iOS / 液态玻璃
// ============================================================================
// 按下：快速放大到 1.055，带一点过冲（弹性）
// 松手：阻尼弹簧弹回 1.00，1~2 次微幅回弹
// 长按：停在放大状态，不会继续涨
struct PressState {
    float cur = 1.0f;
    float vel = 0.0f;
    int   held = 0;
};

PressState g_press[32];

constexpr float kPressSpringK = 320.0f;
constexpr float kPressDamp    = 22.0f;
constexpr float kPressScale   = 1.055f;

float PressScale(int slot, bool down, float dt) {
    if (slot < 0 || slot >= 32) return down ? kPressScale : 1.0f;
    PressState& ps = g_press[slot];
    if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;

    const float target = down ? kPressScale : 1.0f;
    const float k = down ? (kPressSpringK * 1.6f) : kPressSpringK;
    const float c = down ? (kPressDamp * 1.5f)    : kPressDamp;

    // 半隐式欧拉积分（比显式稳定）
    const float accel = -k * (ps.cur - target) - c * ps.vel;
    ps.vel += accel * dt;
    ps.cur += ps.vel * dt;

    if (fabsf(ps.cur - target) < 0.0006f && fabsf(ps.vel) < 0.006f) {
        ps.cur = target;
        ps.vel = 0.0f;
    }
    if (ps.cur < 0.90f) ps.cur = 0.90f;
    if (ps.cur > 1.12f) ps.cur = 1.12f;

    ps.held = down ? 1 : 0;
    return ps.cur;
}

void PressRect(ImVec2& a, ImVec2& b, float scale) {
    if (scale <= 0.0001f) return;
    if (fabsf(scale - 1.0f) < 0.0008f) return;
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float hw = (b.x - a.x) * 0.5f * scale;
    const float hh = (b.y - a.y) * 0.5f * scale;
    a.x = cx - hw;  a.y = cy - hh;
    b.x = cx + hw;  b.y = cy + hh;
}

ImU32 PressLighten(ImU32 col, float scale) {
    const float t = (scale - 1.0f) / (kPressScale - 1.0f);
    if (t <= 0.01f) return col;
    const ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
    const float k = 0.20f * (t > 1.0f ? 1.0f : t);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(c.x + (1.0f - c.x) * k,
               c.y + (1.0f - c.y) * k,
               c.z + (1.0f - c.z) * k,
               c.w));
}

// 带黑色阴影的文本：先画一层偏移黑字，再画本体
void TxtS(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* txt,
          float ox = 1.0f, float oy = 1.5f) {
    if (!dl || !txt) return;
    dl->AddText(ImVec2(pos.x + ox, pos.y + oy), IM_COL32(0, 0, 0, 190), txt);
    dl->AddText(pos, col, txt);
}

void UiText(Ui& u, const char* txt, ImU32 col, float dx = 0.0f) {
    TxtS(u.dl, ImVec2(u.x0 + dx, u.y - u.sy), col, txt);
}

void UiSep(Ui& u) {
    const float yy = u.y - u.sy + 4.0f * u.s;
    u.dl->AddLine(ImVec2(u.x0, yy), ImVec2(u.x0 + u.w, yy), g_theme.sep, 1.0f);
    u.y += 10.0f * u.s;
}

// 分段标题
void UiSection(Ui& u, const char* title) {
    UiText(u, title, g_theme.accent);
    u.y += u.th + 5.0f * u.s;
}

// 说明文字（灰色小字，自动按可用宽度换行）
void UiNote(Ui& u, const char* txt) {
    const float wrap = u.w;
    const ImVec2 sz = ImGui::CalcTextSize(txt, nullptr, false, wrap);
    u.dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                  ImVec2(u.x0, u.y - u.sy), g_theme.textDim, txt, nullptr, wrap);
    u.y += sz.y + 6.0f * u.s;
}

// iOS 开关
bool UiSwitch(Ui& u, const char* label, bool* v, int slot) {
    const float h  = 30.0f * u.s;
    const float sw = 52.0f * u.s;
    const float kr = (h * 0.5f) - 2.5f * u.s;
    const float mg = 4.0f * u.s;
    const float rowH = (h > 36.0f * u.s) ? h : 36.0f * u.s;

    // 命中区（文字 + 开关整行可点）
    ImGui::SetCursorScreenPos(ImVec2(u.x0, u.y - u.sy));
    ImGui::PushID(slot);
    ImGui::InvisibleButton("sw", ImVec2(u.w, rowH));
    ImGui::PopID();
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) { *v = !*v; ui::save(); }

    // 开关动画：线性推进 + cubic-bezier 缓动
    float tRaw = (*v) ? 1.0f : 0.0f;
    const bool swHeld = hovered && ImGui::IsMouseDown(0);
    const float swPt = PressScale(11, swHeld, g_lastDt);

    {
        const int   si = SwSlot(slot, 120);
        const float target = (*v) ? 1.0f : 0.0f;
        const float dt = (g_lastDt > 0.0f && g_lastDt < 0.1f) ? g_lastDt : 0.016f;
        const float step = dt / 0.22f;            // 220ms 走完
        // 首次使用：直接对齐到目标值，避免从 0 开始"补动画"
        static bool s_seeded[kSwAnimN] = { false };
        if (!s_seeded[si]) {
            s_seeded[si] = true;
            g_swAnim[si] = target;
        }
        if (g_swAnim[si] < target) {
            g_swAnim[si] += step;
            if (g_swAnim[si] > target) g_swAnim[si] = target;
        } else if (g_swAnim[si] > target) {
            g_swAnim[si] -= step;
            if (g_swAnim[si] < target) g_swAnim[si] = target;
        }
        tRaw = g_swAnim[si];
    }
    // cubic-bezier(0.34, 1.56, 0.64, 1) —— 轻微过冲
    const float t = BezierAxis(tRaw, 0.34f, 0.64f);

    const float ty = u.y - u.sy + (rowH - u.th) * 0.5f;
    TxtS(u.dl, ImVec2(u.x0, ty), g_theme.text, label);

    const float capX = u.x0 + u.w - sw;
    const float capY = u.y - u.sy + (rowH - h) * 0.5f;

    const ImU32 off = hovered ? g_theme.swOffHover : g_theme.swOff;
    ImU32 bg = off;
    if (t > 0.001f && t < 0.999f) {
        const ImVec4 a = ImGui::ColorConvertU32ToFloat4(off);
        const ImVec4 b = ImGui::ColorConvertU32ToFloat4(g_theme.swOn);
        bg = ImGui::ColorConvertFloat4ToU32(
            ImVec4(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t), 1.0f));
    } else if (t >= 0.999f) {
        bg = g_theme.swOn;
    }
    bg = PressLighten(bg, swPt);
    u.dl->AddRectFilled(ImVec2(capX, capY), ImVec2(capX + sw, capY + h),
                        bg, h * 0.5f);

    const float kx = capX + mg + kr + (sw - mg * 2.0f - kr * 2.0f) * t;
    const float ky = capY + h * 0.5f;
    u.dl->AddCircleFilled(ImVec2(kx + 0.5f, ky + 1.5f), kr, IM_COL32(0, 0, 0, 55), 28);
    u.dl->AddCircleFilled(ImVec2(kx, ky), kr, g_theme.knob, 28);

    u.y += rowH + 4.0f * u.s;
    return clicked;
}

// 滑条（自绘）
void UiSlider(Ui& u, const char* label, float* v, float lo, float hi, int id) {
    {
        const float top = u.y - u.sy;
        TxtS(u.dl, ImVec2(u.x0, top), g_theme.textDim, label);

        char vb[32];
        if (hi - lo >= 20.0f) snprintf(vb, sizeof(vb), "%.0f", *v);
        else                  snprintf(vb, sizeof(vb), "%.2f", *v);
        const ImVec2 ts = ImGui::CalcTextSize(vb);
        TxtS(u.dl, ImVec2(u.x0 + u.w - ts.x, top), g_theme.accent, vb);
    }
    u.y += u.th + 3.0f * u.s;

    const float h  = 26.0f * u.s;
    const float tr = 9.0f * u.s;
    const float kr = 9.0f * u.s;

    ImGui::SetCursorScreenPos(ImVec2(u.x0, u.y - u.sy));
    ImGui::PushID(id);
    ImGui::InvisibleButton("sl", ImVec2(u.w, h));
    ImGui::PopID();
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    if (active) {
        g_dragSlider = id;
    } else if (g_dragSlider == id && !ImGui::IsMouseDown(0)) {
        g_dragSlider = -1;
        ui::save();
    }

    const float top = u.y - u.sy;
    const float cy  = top + h * 0.5f;
    const float tx0 = u.x0 + kr;
    const float tx1 = u.x0 + u.w - kr;
    const float span = (tx1 > tx0) ? (tx1 - tx0) : 1.0f;

    if (g_dragSlider == id && ImGui::IsMouseDown(0)) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float a = Clamp01((mx - tx0) / span);
        *v = lo + (hi - lo) * a;
    }
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;

    const float a = (hi > lo) ? ((*v - lo) / (hi - lo)) : 0.0f;
    const float kx = tx0 + span * a;

    const ImU32 trBg = UseDark() ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 26);
    u.dl->AddRectFilled(ImVec2(tx0, cy - tr * 0.5f), ImVec2(tx1, cy + tr * 0.5f),
                        trBg, tr * 0.5f);
    if (kx > tx0 + 0.5f) {
        u.dl->AddRectFilled(ImVec2(tx0, cy - tr * 0.5f), ImVec2(kx, cy + tr * 0.5f),
                            g_theme.accent, tr * 0.5f);
    }
    const ImU32 kc = (hovered || active) ? g_theme.accent : g_theme.knob;
    u.dl->AddCircleFilled(ImVec2(kx, cy + 1.0f), kr + 1.0f, IM_COL32(0, 0, 0, 60), 26);
    u.dl->AddCircleFilled(ImVec2(kx, cy), kr, kc, 26);
    if (hovered || active) {
        u.dl->AddCircleFilled(ImVec2(kx, cy), kr * 0.55f, IM_COL32(255, 255, 255, 235), 22);
    }

    u.y += h + 5.0f * u.s;
}

// 自绘按钮（带按压反馈）
bool UiButton(Ui& u, const char* label, float hDip, int slot) {
    const float rowH = hDip * u.s;
    const float top  = u.y - u.sy;

    ImGui::SetCursorScreenPos(ImVec2(u.x0, top));
    ImGui::PushID(slot);
    ImGui::InvisibleButton("btn", ImVec2(u.w, rowH));
    ImGui::PopID();
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = hovered && ImGui::IsMouseDown(0);
    const float pt     = PressScale(slot + 16, held, g_lastDt);

    ImVec2 a(u.x0, top), b(u.x0 + u.w, top + rowH);
    PressRect(a, b, pt);

    ImU32 bg = clicked ? g_theme.accent
                       : (hovered ? g_theme.cardHover : g_theme.card);
    bg = PressLighten(bg, pt);
    u.dl->AddRectFilled(a, b, bg, 8.0f * u.s);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    TxtS(u.dl, ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f,
                      a.y + (b.y - a.y - ts.y) * 0.5f), g_theme.text, label);

    u.y += rowH + 6.0f * u.s;
    return clicked;
}

// 键值行（只演示，不做任何副作用）
void UiKV(Ui& u, const char* k, const char* v) {
    const float top = u.y - u.sy;
    TxtS(u.dl, ImVec2(u.x0, top), g_theme.textDim, k);
    const ImVec2 ts = ImGui::CalcTextSize(v);
    TxtS(u.dl, ImVec2(u.x0 + u.w - ts.x, top), g_theme.text, v);
    u.y += u.th + 6.0f * u.s;
}

// 侧滑条
void DrawScrollbar(ImDrawList* dl, float bx, float by, float bw, float bh,
                   float contentH) {
    if (contentH <= bh + 1.0f) return;
    ui::Prefs& p = ui::prefs;
    const float tw = p.scrollW;

    const ImU32 trackCol = UseDark() ? IM_COL32(255, 255, 255, 26) : IM_COL32(0, 0, 0, 24);
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + tw, by + bh), trackCol, tw * 0.5f);

    float kh = bh * (bh / contentH);
    if (kh < 40.0f) kh = 40.0f;
    const float maxScroll = contentH - bh;
    float a = (maxScroll > 0.0f) ? (g_menuScroll / maxScroll) : 0.0f;
    a = Clamp01(a);
    const float ky = by + (bh - kh) * a;
    dl->AddRectFilled(ImVec2(bx, ky), ImVec2(bx + tw, ky + kh),
                      g_theme.accentDim, tw * 0.5f);
}

// ============================================================================
// 分区与标题
// ============================================================================
const char* kTabNames[5] = { "概览", "控件", "布局", "主题", "外观" };

// 诗句库
const char* kPoems[] = {
    "月落乌啼霜满天 江枫渔火对愁眠",
    "停车坐爱枫林晚 霜叶红于二月花",
    "浔阳江头夜送客 枫叶荻花秋瑟瑟",
    "玉露凋伤枫树林 巫山巫峡气萧森",
    "明朝挂帆席 枫叶落纷纷",
    "枫叶千枝复万枝 江桥掩映暮帆迟",
    "日暮秋风起 萧萧枫树林",
    "青枫江上秋帆远 白帝城边古木疏",
    "枫林橘树丹青合 复道重楼锦绣悬",
    "青枫飒飒雨凄凄 秋色遥看入楚迷",
    "山远天高烟水寒 相思枫叶丹",
    "江枫渐老 汀蕙半凋 满目败红衰翠",
    "枫叶落 荻花干 醉宿渔舟不觉寒",
    "游人去后枫林夜 月满空山可奈何",
    "枫叶芦花满钓船 水风清处枕琴眠",
    "枫叶红绡翦 芦花白雪铺",
    "枫岸月斜明 猿啼旅梦惊",
    "枫树隐茅屋 橘林系渔舟",
    "枫林叶堕红 荷盖水披镜",
    "江枫渔火对愁眠",
    "霜叶红于二月花",
    "丹枫索索满林红",
    "枫落吴江冷",
    "枫林霜叶舞",
    "丹枫飒飒送秋声",
    "枫林霜重叶初丹",
    "霜枫染就一江秋",
    "枫叶流丹照水红",
    "枫林秋色染霜红",
    "霜枫红似火"
};
const int kPoemCount = (int)(sizeof(kPoems) / sizeof(kPoems[0]));

// 每开一次菜单换一句（随机，且不与上次重复）
const char* PickPoem(bool reopen) {
    static const char* s_cur = nullptr;
    static int s_last = -1;

    if (s_cur && !reopen) return s_cur;
    if (kPoemCount <= 0) return "";
    if (kPoemCount == 1) { s_cur = kPoems[0]; return s_cur; }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned seed = (unsigned)(ts.tv_nsec ^ (ts.tv_sec * 2654435761u));
    static unsigned s_extra = 0;
    seed ^= (s_extra++ * 2246822519u);
    seed ^= seed >> 13;
    seed *= 0x5bd1e995u;
    seed ^= seed >> 15;

    int idx = s_last;
    for (int tries = 0; tries < 8; ++tries) {
        idx = (int)(seed % (unsigned)kPoemCount);
        if (idx != s_last) break;
        seed = seed * 1664525u + 1013904223u;
    }
    if (idx == s_last) idx = (s_last + 1) % kPoemCount;
    s_last = idx;
    s_cur = kPoems[idx];
    return s_cur;
}

// ============================================================================
// 分区页 —— 全部是 UI 演示，没有任何游戏操作
// ============================================================================
void PageOverview(Ui& u) {
    UiSection(u, "这是什么");
    UiNote(u,
        "一套可以直接挂到任意 Android + OpenGL ES 应用上的 ImGui 覆盖层界面。"
        "主题、控件、分页、悬浮球、模糊、缩放全部是原版实现，"
        "功能侧的游戏逻辑已经整体摘除。");
    UiSep(u);

    UiSection(u, "操作方式");
    UiKV(u, "点悬浮球", "打开菜单");
    UiKV(u, "点左上图片", "关闭菜单");
    UiKV(u, "长按图片拖动", "移动菜单位置");
    UiKV(u, "右下角斜纹", "自由缩放面板");
    UiKV(u, "拖侧滑条", "内容区滚动");
    UiSep(u);

    UiSection(u, "编译期开关");
    UiKV(u, "界面缩放", "外观 → 整体缩放");
    UiKV(u, "配置落盘", ui::kPath);
    UiSep(u);

    if (UiButton(u, "保存当前偏好到文件", 38.0f, 900)) {
        const bool ok = ui::save();
        FLCY_LOG("save prefs -> %s", ok ? "ok" : "failed");
    }
    if (UiButton(u, "恢复默认外观", 38.0f, 901)) {
        ui::reset();
        ui::save();
    }
}

void PageWidgets(Ui& u) {
    static bool  s_a = true, s_b = false, s_c = true;
    static float s_slider = 0.65f;
    static float s_int    = 6.0f;
    static int   s_clicks = 0;

    UiSection(u, "开关");
    UiSwitch(u, "开关 A（默认开）", &s_a, 0);
    UiSwitch(u, "开关 B（默认关）", &s_b, 1);
    UiSwitch(u, "开关 C", &s_c, 2);
    UiNote(u, "开关本身带 220ms 贝塞尔过冲与弹簧按压反馈，槽位独立。");
    UiSep(u);

    UiSection(u, "滑条");
    UiSlider(u, "连续值", &s_slider, 0.0f, 1.0f, 30);
    UiSlider(u, "整数刻度", &s_int, 1.0f, 12.0f, 31);
    UiSep(u);

    UiSection(u, "按钮");
    if (UiButton(u, "点我（有按压动效）", 38.0f, 902)) ++s_clicks;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d 次", s_clicks);
    UiKV(u, "点击计数", buf);
    UiSep(u);

    UiSection(u, "文本");
    UiText(u, "普通行文本（带阴影）", g_theme.text);
    u.y += u.th + 4.0f * u.s;
    UiText(u, "次要文本", g_theme.textDim);
    u.y += u.th + 4.0f * u.s;
    UiText(u, "主题色文本", g_theme.accent);
    u.y += u.th + 8.0f * u.s;
    UiNote(u,
        "说明段落会自动按内容宽度换行，适合放长一点的中文描述，"
        "不用自己算断行位置。");
}

void PageLayout(Ui& u) {
    ui::Prefs& p = ui::prefs;

    UiSection(u, "面板");
    UiSlider(u, "面板圆角", &p.menuRound, 0.0f, 60.0f, 40);
    UiSlider(u, "分区圆角", &p.tabRound, 0.0f, 26.0f, 41);
    UiSlider(u, "整体缩放", &p.uiScale, 0.75f, 1.50f, 42);
    UiSlider(u, "不透明度", &p.bgAlpha, 0.45f, 1.0f, 43);
    UiSep(u);

    UiSection(u, "内容");
    UiSlider(u, "侧滑条粗细", &p.scrollW, 6.0f, 30.0f, 44);
    UiSlider(u, "图片圆角", &p.imgRound, 0.0f, 0.5f, 45);
    UiSep(u);

    UiSection(u, "尺寸");
    UiKV(u, "自适应", (p.menuW <= 1.0f) ? "开（4:3）" : "关（手动）");
    if (UiButton(u, "恢复自适应尺寸", 38.0f, 903)) {
        p.menuW = 0.0f;
        p.menuH = 0.0f;
        p.menuX = -1.0f;
        p.menuY = -1.0f;
        ui::save();
    }
    UiNote(u, "右下角斜纹可以直接拖，松手即落盘；这里只做复位。");
}

void PageTheme(Ui& u) {
    ui::Prefs& p = ui::prefs;

    UiSection(u, "模式");
    UiSwitch(u, "黑色主题", &p.forceBlack, 3);
    if (!p.forceBlack) {
        UiSwitch(u, p.darkTheme ? "深色（白字）" : "浅色（黑字）", &p.darkTheme, 4);
    }
    UiNote(u,
        "黑色主题 = 纯黑面板白字；关掉后按深色/浅色两种面板走。"
        "主题色选「纯黑」时 accent 会自动反相，避免黑底上看不见。");
    UiSep(u);

    UiSection(u, "主题色");
    {
        const float sz = 40.0f * u.s;
        for (int i = 0; i < 6; ++i) {
            const ImVec2 pos(u.x0 + i * (sz + 8.0f * u.s), u.y - u.sy);
            ImGui::SetCursorScreenPos(pos);
            ImGui::PushID(200 + i);
            ImGui::InvisibleButton("swatch", ImVec2(sz, sz));
            ImGui::PopID();
            if (ImGui::IsItemClicked()) {
                p.accentHue = ui::kSwatches[i].h;
                p.accentSat = ui::kSwatches[i].s;
                ui::save();
            }
            const bool sel = (fabsf(p.accentHue - ui::kSwatches[i].h) < 0.003f &&
                              fabsf(p.accentSat - ui::kSwatches[i].s) < 0.02f);
            const bool isBlack = (ui::kSwatches[i].s < 0.01f);
            const float val = isBlack ? 0.06f : 0.94f;
            const ImU32 c = ui::hsv(ui::kSwatches[i].h, ui::kSwatches[i].s, val, 1.0f);
            const ImVec2 ctr(pos.x + sz * 0.5f, pos.y + sz * 0.5f);
            u.dl->AddCircleFilled(ctr, sz * 0.46f, c, 32);
            if (isBlack) {
                u.dl->AddCircle(ctr, sz * 0.46f, IM_COL32(255, 255, 255, 110), 32, 1.5f);
            }
            if (sel) {
                u.dl->AddCircle(ctr, sz * 0.46f + 2.0f,
                                IM_COL32(255, 255, 255, 230), 32, 2.5f);
            }
        }
        u.y += sz + 12.0f * u.s;
    }
    UiSep(u);
    UiSection(u, "预览");
    UiKV(u, "主题色", "见上方色板");
    UiKV(u, "面板色", p.forceBlack ? "纯黑" : (p.darkTheme ? "深灰" : "浅色"));
}

void PageAppearance(Ui& u) {
    ui::Prefs& p = ui::prefs;

    UiSection(u, "模糊");
    UiSwitch(u, "启用菜单模糊", &p.blurOn, 0);
    u.y += 6.0f * u.s;
    if (p.blurOn) {
        UiSlider(u, "模糊强度", &p.blurAmount, 0.20f, 4.00f, 30);
        UiSlider(u, "模糊阈值", &p.blurThreshold, 0.00f, 1.00f, 31);
    }
    UiSep(u);

    UiSection(u, "悬浮球");
    UiSlider(u, "球体大小", &p.ballScale, 0.60f, 1.80f, 20);
    UiSlider(u, "球体不透明度", &p.ballAlpha, 0.0f, 1.0f, 21);
    UiSep(u);

    UiSection(u, "落盘");
    UiKV(u, "配置路径", ui::kPath);
    if (UiButton(u, "立即保存", 38.0f, 904)) ui::save();
    if (UiButton(u, "恢复默认外观", 38.0f, 905)) {
        ui::reset();
        ui::save();
    }
}

void PagePlaceholder(Ui& u, const char* title, const char* note) {
    UiSection(u, title);
    UiSep(u);
    if (note && note[0]) {
        UiText(u, note, g_theme.textDim);
        u.y += u.th + 5.0f * u.s;
    }
}

// ============================================================================
// 主菜单
// ============================================================================
void DrawMenu(float W, float H, float dt) {
    const bool closing = (!g_menuOpen && g_animT > 0.001f);
    if (g_animT <= 0.002f && !closing) return;

    ui::Prefs& p = ui::prefs;
    BuildTheme();

    float mx, my, mw, mh;
    MenuRect(W, H, mx, my, mw, mh);

    const float round = p.menuRound;

    // ---- 动画 ----
    // 开：decelerate（快起长尾）；关：accelerate（缓起快收）
    const float eo = g_menuOpen ? EaseDecel(g_animT)
                                : (1.0f - EaseAccel(Clamp01(1.0f - g_animT)));
    const float ec = Clamp01(eo);
    float bx, by;
    BallCenter(W, H, bx, by);
    const float panCx = mx + mw * 0.5f;
    const float panCy = my + mh * 0.5f;

    const float sc = Lerp(0.10f, 1.0f, ec);
    const float centerX = Lerp(bx, panCx, ec);
    const float centerY = Lerp(by, panCy, ec);

    const float animW = mw * sc;
    const float animH = mh * sc;
    const float animX = centerX - animW * 0.5f;
    const float animY = centerY - animH * 0.5f;

    // alpha 比几何早到位：文字不能"飘着"出现
    const float alphaT = Clamp01(g_animT * 1.8f);
    const float alpha = Clamp01(g_menuOpen ? EaseOutCubic(alphaT)
                                           : (1.0f - EaseAccel(Clamp01(1.0f - alphaT))));

    ImGui::SetNextWindowPos(ImVec2(animX, animY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(animW, animH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, round);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;

    ImDrawList* dl = nullptr;

    if (ImGui::Begin("##imgui_ui_menu", nullptr, flags)) {
        dl = ImGui::GetWindowDrawList();

        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();

        // 面板底
        //   模糊开        -> 模糊纹理之上压半透明黑（压暗但透出模糊）
        //   模糊关+黑主题 -> 不透明纯黑
        //   模糊关+非黑   -> 深灰或浅色实心面板
        if (p.blurOn) {
            const int a = (int)(255.0f * (0.42f + 0.22f * p.bgAlpha));
            dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                              IM_COL32(0, 0, 0, a), round);
        } else {
            dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                              g_theme.panel, round);
        }

        // 主题色边环：只画描边，不二次填充面板
        // （二次填充会再叠一次 alpha，把模糊盖死）
        {
            const float inset = 5.0f;
            const float ringW = 3.0f;
            const float oR = round - inset;
            if (oR > 0.5f) {
                const ImVec2 oMin(wp.x + inset, wp.y + inset);
                const ImVec2 oMax(wp.x + ws.x - inset, wp.y + ws.y - inset);
                dl->AddRect(ImVec2(oMin.x + ringW * 0.5f, oMin.y + ringW * 0.5f),
                            ImVec2(oMax.x - ringW * 0.5f, oMax.y - ringW * 0.5f),
                            g_theme.accent, oR - ringW * 0.5f, 0, ringW);
            }
        }

        const float pad = 14.0f * p.uiScale;
        float imgSz = ws.y * 0.22f;
        {
            const float imax = ws.x * 0.22f;
            if (imgSz > imax) imgSz = imax;
            if (imgSz < 54.0f) imgSz = 54.0f;
        }
        const float sideW = imgSz + pad * 2.0f;
        const float imgX = wp.x + pad;
        const float imgY = wp.y + pad;

        // 侧栏：用"更浅的一层"表现分区，而不是再糊一层不透明底
        {
            const ImU32 sideTint = UseDark() ? IM_COL32(255, 255, 255, 12)
                                             : IM_COL32(0, 0, 0, 12);
            dl->AddRectFilled(wp, ImVec2(wp.x + sideW, wp.y + ws.y),
                              sideTint, round, ImDrawFlags_RoundCornersLeft);
        }
        dl->AddLine(ImVec2(wp.x + sideW, wp.y + 12.0f),
                    ImVec2(wp.x + sideW, wp.y + ws.y - 12.0f),
                    g_theme.sep, 1.0f);

        // 图片
        const float ir = imgSz * p.imgRound * 0.5f;
        const uint32_t tex = ui_layer::BallTexture();
        if (tex) {
            dl->AddImageRounded((ImTextureID)tex,
                ImVec2(imgX, imgY), ImVec2(imgX + imgSz, imgY + imgSz),
                ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), ir, 0);
        } else {
            dl->AddRectFilled(ImVec2(imgX, imgY), ImVec2(imgX + imgSz, imgY + imgSz),
                              g_theme.card, ir);
        }
        dl->AddRect(ImVec2(imgX, imgY), ImVec2(imgX + imgSz, imgY + imgSz),
                    g_theme.sep, ir, 0, 1.0f);

        // 图片热区（长按拖 / 轻点关）
        ImGui::SetCursorScreenPos(ImVec2(imgX, imgY));
        ImGui::PushID(400);
        ImGui::InvisibleButton("img", ImVec2(imgSz, imgSz));
        ImGui::PopID();

        // ---- 内容区顶部诗句 ----
        const float cx0 = wp.x + sideW + pad;
        const float cwFull = ws.x - (sideW + pad * 2.0f);
        const float sbarW = p.scrollW + 8.0f;
        const float cw = cwFull - sbarW;

        float yTop = wp.y + pad;
        {
            // 菜单"从关到开"的瞬间换诗
            static bool s_wasOpen = false;
            const bool justOpened = (g_menuOpen && !s_wasOpen);
            s_wasOpen = g_menuOpen;
            const char* verse = PickPoem(justOpened);
            const float fs = ImGui::GetFontSize();
            const ImVec2 ts = ImGui::CalcTextSize(verse);
            float fsc = 1.0f;
            if (ts.x > cw && cw > 10.0f) fsc = cw / ts.x;
            if (fsc < 0.999f) {
                dl->AddText(ImGui::GetFont(), fs * fsc, ImVec2(cx0, yTop),
                            g_theme.text, verse);
            } else {
                TxtS(dl, ImVec2(cx0, yTop), g_theme.text, verse);
            }
            const float lw = ts.x * fsc * 0.62f;
            const float ly = yTop + ts.y * fsc + 8.0f;
            dl->AddLine(ImVec2(cx0, ly), ImVec2(cx0 + lw, ly), g_theme.accent, 2.5f);

            // ---- 欢迎语：落在线的下方 ----
            {
                const char* welcome = "ImGui 覆盖层界面框架";
                const float wfs = ImGui::GetFontSize() * 0.82f;
                const ImVec2 wts = ImGui::CalcTextSize(welcome);
                float wsc = 1.0f;
                if (wts.x > cw && cw > 10.0f) wsc = cw / wts.x;
                const float wy = ly + 7.0f;
                if (wsc < 0.999f) {
                    dl->AddText(ImGui::GetFont(), wfs * wsc,
                                ImVec2(cx0, wy), g_theme.textDim, welcome);
                    yTop = wy + wts.y * wsc + 8.0f;
                } else {
                    dl->AddText(ImGui::GetFont(), wfs,
                                ImVec2(cx0 + 1.0f, wy + 1.5f),
                                IM_COL32(0, 0, 0, 190), welcome);
                    dl->AddText(ImGui::GetFont(), wfs,
                                ImVec2(cx0, wy), g_theme.textDim, welcome);
                    yTop = wy + wts.y + 8.0f;
                }
            }
        }

        // ---- 分区按钮 ----
        // 加一层面板内缩裁剪：功能块是直角矩形，贴近面板底/右时外角会
        // 露出圆角轮廓，动画中尤其明显。
        PushPanelClip(dl, wp, ws, round, pad);

        const float blockY0 = imgY + imgSz + pad * 1.3f;
        const float blockH  = 44.0f * p.uiScale;
        const float blockGap= 8.0f * p.uiScale;
        const float blockW  = sideW - pad * 2.0f;

        const int kTabCount = 5;
        for (int i = 0; i < kTabCount; ++i) {
            const float by = blockY0 + i * (blockH + blockGap);
            if (by + blockH > wp.y + ws.y - pad) {
                const float avail = (wp.y + ws.y - pad) - by;
                if (avail < 20.0f) break;
            }

            const ImVec2 bmin(wp.x + pad, by);
            const ImVec2 bmax(bmin.x + blockW, by + blockH);

            ImGui::SetCursorScreenPos(bmin);
            ImGui::PushID(500 + i);
            ImGui::InvisibleButton("tab", ImVec2(blockW, blockH));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive() && ImGui::IsMouseDown(0);
            const float pt = PressScale(i, held, g_lastDt);
            if (ImGui::IsItemClicked() && g_tab != i) {
                g_prevTab = g_tab;
                g_tabDir = (i > g_tab) ? 1 : -1;
                g_tab = i;
                g_pageX = 0.0f;
                g_menuScroll = 0.0f;
            }

            const bool act = (g_tab == i);
            ImVec2 pmin = bmin, pmax = bmax;
            PressRect(pmin, pmax, pt);

            ImU32 bgc = act ? g_theme.accentDim
                            : (hov ? g_theme.cardHover : g_theme.card);
            bgc = PressLighten(bgc, pt);
            dl->AddRectFilled(pmin, pmax, bgc, p.tabRound);
            if (act) {
                const float ins = p.tabRound;
                dl->AddRectFilled(ImVec2(bmin.x, bmin.y + ins),
                                  ImVec2(bmin.x + 3.5f, bmax.y - ins),
                                  g_theme.accent, 2.0f);
            }

            const ImU32 tc = act ? IM_COL32(255, 255, 255, 255) : g_theme.text;
            const ImVec2 ts = ImGui::CalcTextSize(kTabNames[i]);
            // 文字跟着缩放后的矩形居中
            TxtS(dl, ImVec2(pmin.x + (pmax.x - pmin.x - ts.x) * 0.5f,
                            pmin.y + (pmax.y - pmin.y - ts.y) * 0.5f),
                 tc, kTabNames[i]);
        }

        dl->PopClipRect();   // 结束 PushPanelClip

        // ---- 内容区 ----
        const float cy0 = yTop;
        const float ch = (wp.y + ws.y - pad) - cy0;
        if (ch > 30.0f && cw > 30.0f) {
            // 分区切换：整块竖向滚过去。两页同时存在，位置同步插值。
            g_pageX = Clamp01(g_pageX + dt / 0.30f);

            const float pgr = EaseSmooth(g_pageX);
            const float dir = (g_tabDir >= 0) ? 1.0f : -1.0f;
            const float oldDy = -dir * ch * pgr;
            const float newDy = dir * ch * (1.0f - pgr);

            dl->PushClipRect(ImVec2(cx0, cy0), ImVec2(cx0 + cwFull, cy0 + ch), true);

            auto drawPage = [&](int tab, float dy) -> float {
                Ui u;
                u.dl = dl;
                u.x0 = cx0;
                u.y0 = cy0;
                u.w = cw;
                u.s = p.uiScale;
                u.th = ImGui::GetTextLineHeight();
                u.sy = (tab == g_tab) ? g_menuScroll : 0.0f;
                u.y = cy0 + dy;

                const float yBefore = u.y;

                switch (tab) {
                    case 0: PageOverview(u); break;
                    case 1: PageWidgets(u); break;
                    case 2: PageLayout(u); break;
                    case 3: PageTheme(u); break;
                    case 4: PageAppearance(u); break;
                    default: PagePlaceholder(u, "空分区", "没有内容"); break;
                }
                return u.y - yBefore;   // contentH
            };

            float contentH = 0.0f;
            if (g_pageX < 0.999f && g_prevTab >= 0 && g_prevTab != g_tab) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f - pgr);
                drawPage(g_prevTab, oldDy);
                ImGui::PopStyleVar();
            }
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f + 0.65f * pgr);
            contentH = drawPage(g_tab, newDy);
            ImGui::PopStyleVar();

            if (g_pageX >= 0.999f) g_prevTab = -1;

            const float maxScroll = (contentH > ch) ? (contentH - ch) : 0.0f;
            if (g_menuScroll > maxScroll) g_menuScroll = maxScroll;
            if (g_menuScroll < 0.0f) g_menuScroll = 0.0f;

            dl->PopClipRect();

            // 内容区滚动：拖侧滑条 / 滚轮 / 在空白处上下拖
            {
                const float sbX = cx0 + cwFull - p.scrollW - 2.0f;
                const float sbW = p.scrollW;
                float kh = (contentH > ch) ? ch * (ch / contentH) : ch;
                if (kh < 40.0f) kh = 40.0f;
                const float ky = cy0 + (ch - kh) * (maxScroll > 0.0f ? (g_menuScroll / maxScroll) : 0.0f);

                ImGui::SetCursorScreenPos(ImVec2(sbX, maxScroll > 0.0f ? ky : cy0));
                ImGui::PushID(700);
                ImGui::InvisibleButton("sbar",
                    ImVec2(sbW, maxScroll > 0.0f ? kh : 1.0f));
                ImGui::PopID();

                if (ImGui::IsItemActive() && maxScroll > 0.0f) {
                    const float my2 = ImGui::GetIO().MousePos.y;
                    float a = (my2 - cy0 - kh * 0.5f) / (ch - kh);
                    a = Clamp01(a);
                    g_menuScroll = a * maxScroll;
                } else if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                    const float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel != 0.0f) {
                        g_menuScroll -= wheel * 60.0f;
                        if (g_menuScroll < 0.0f) g_menuScroll = 0.0f;
                        if (g_menuScroll > maxScroll) g_menuScroll = maxScroll;
                    }
                }
            }

            DrawScrollbar(dl, cx0 + cwFull - p.scrollW - 2.0f, cy0,
                          p.scrollW, ch, contentH);
        }

        // ---- 右下角缩放手柄 ----
        const float hs = 26.0f * p.uiScale;
        const ImVec2 hmin(wp.x + ws.x - hs - 6.0f, wp.y + ws.y - hs - 6.0f);
        ImGui::SetCursorScreenPos(hmin);
        ImGui::PushID(600);
        ImGui::InvisibleButton("resize", ImVec2(hs, hs));
        ImGui::PopID();
        const bool rHov = ImGui::IsItemHovered();
        const bool rAct = ImGui::IsItemActive();
        for (int i = 0; i < 3; ++i) {
            const float o = 8.0f + i * 5.0f;
            const ImU32 c = (rHov || rAct) ? g_theme.accent : g_theme.sep;
            dl->AddLine(ImVec2(wp.x + ws.x - o, wp.y + ws.y - 8.0f),
                        ImVec2(wp.x + ws.x - 8.0f, wp.y + ws.y - o), c, 2.0f);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);

    if (dl) {
        // 模糊底画在背景层（相对独立的 DrawList），单独按当前动画状态处理
        DrawMenuBlur(H, animX, animY, animW, animH, round * sc, g_animT, alpha);

        // 顶点变换：整块菜单（含所有自绘内容，因为都在同一张 DrawList）
        if (sc < 0.9985f || fabsf(centerX - panCx) > 0.01f ||
            fabsf(centerY - panCy) > 0.01f) {
            const int n = dl->VtxBuffer.Size;
            for (int i = 0; i < n; ++i) {
                ImDrawVert& v = dl->VtxBuffer.Data[i];
                v.pos.x = centerX + (v.pos.x - centerX) * sc;
                v.pos.y = centerY + (v.pos.y - centerY) * sc;
            }

            // 关键：变换完之后必须把裁剪矩形一起做同样的仿射变换。
            // 粗暴地全放开会取消内容区裁剪，功能块在动画中会溢出面板外。
            for (int i = 0; i < dl->CmdBuffer.Size; ++i) {
                ImDrawCmd& c = dl->CmdBuffer[i];
                ImVec4& r = c.ClipRect;
                r.x = centerX + (r.x - centerX) * sc;
                r.y = centerY + (r.y - centerY) * sc;
                r.z = centerX + (r.z - centerX) * sc;
                r.w = centerY + (r.w - centerY) * sc;
            }
            if (dl->_ClipRectStack.Size > 0) {
                ImVec4& r = dl->_ClipRectStack.back();
                r.x = centerX + (r.x - centerX) * sc;
                r.y = centerY + (r.y - centerY) * sc;
                r.z = centerX + (r.z - centerX) * sc;
                r.w = centerY + (r.w - centerY) * sc;
            }
        }
    }
}

// ============================================================================
// 交互
// ============================================================================
void StepAnim(bool open, float dt) {
    const float d = dt / (open ? kOpenSec : kCloseSec);
    g_animT += open ? d : -d;
    if (g_animT < 0.0f) g_animT = 0.0f;
    if (g_animT > 1.0f) g_animT = 1.0f;
}

void HandleInteraction(float W, float H, float dt) {
    ImGuiIO& io = ImGui::GetIO();
    const bool valid = ImGui::IsMousePosValid();
    const bool down = valid && io.MouseDown[0];
    const ImVec2 m = io.MousePos;

    if (!g_menuOpen) {
        float cx, cy;
        BallCenter(W, H, cx, cy);
        const float hitR = BallSize(W, H) * 0.5f * 1.30f;
        bool inside = false;
        if (valid) {
            const float dx = m.x - cx, dy = m.y - cy;
            inside = (dx * dx + dy * dy) <= hitR * hitR;
        }
        if (down && !g_ballWasDown && inside) {
            g_dragging = true;
            g_ballMoved = false;
            g_dragDX = m.x - cx;
            g_dragDY = m.y - cy;
        }
        if (g_dragging && down) {
            float nx = m.x - g_dragDX, ny = m.y - g_dragDY;
            const float rad = BallSize(W, H) * 0.5f + 8.0f;
            if (nx < rad) nx = rad;
            if (nx > W - rad) nx = W - rad;
            if (ny < rad) ny = rad;
            if (ny > H - rad) ny = H - rad;
            if (fabsf(nx - ui::prefs.ballX * W) > 6.0f ||
                fabsf(ny - ui::prefs.ballY * H) > 6.0f) g_ballMoved = true;
            ui::prefs.ballX = nx / W;
            ui::prefs.ballY = ny / H;
        }
        if (g_dragging && !down) {
            g_dragging = false;
            if (g_ballMoved) ui::save();
            else g_menuOpen = true;
        }
        g_ballWasDown = down;
        g_imgWasDown = false;
        g_imgHoldT = 0.0f;
        g_imgDragging = false;
        g_imgMoved = false;
        g_resizing = false;
    } else {
        ui::Prefs& p = ui::prefs;
        float mx, my, mw, mh;
        MenuRect(W, H, mx, my, mw, mh);
        const float pad = 14.0f * p.uiScale;
        float imgSz = mh * 0.22f;
        {
            const float imax = mw * 0.22f;
            if (imgSz > imax) imgSz = imax;
            if (imgSz < 54.0f) imgSz = 54.0f;
        }
        const float imgX = mx + pad, imgY = my + pad;

        const bool overImg = valid &&
            m.x >= imgX && m.x <= imgX + imgSz &&
            m.y >= imgY && m.y <= imgY + imgSz;

        if (down && overImg && !g_imgDragging) {
            g_imgHoldT += dt;
            if (g_imgHoldT >= kHoldSec) {
                g_imgDragging = true;
                g_imgMoved = false;
                g_imgDragDX = m.x - mx;
                g_imgDragDY = m.y - my;
            }
        }
        if (!down) g_imgHoldT = 0.0f;

        if (g_imgDragging && down) {
            float nx = m.x - g_imgDragDX, ny = m.y - g_imgDragDY;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + mw > W) nx = W - mw;
            if (ny + mh > H) ny = H - mh;
            if (fabsf(nx - mx) > 2.0f || fabsf(ny - my) > 2.0f) g_imgMoved = true;
            p.menuX = nx;
            p.menuY = ny;
        }
        if (g_imgDragging && !down) {
            g_imgDragging = false;
            g_imgHoldT = 0.0f;
            if (g_imgMoved) ui::save();
        }

        // 轻点图片关闭
        if (!down && g_imgWasDown && overImg && !g_imgDragging && !g_imgMoved) {
            g_menuOpen = false;
        }
        if (!down && !g_imgDragging) g_imgMoved = false;

        // 右下角自由缩放
        const float hs = 26.0f * p.uiScale;
        const bool overHandle = valid &&
            m.x >= mx + mw - hs - 6.0f && m.y >= my + mh - hs - 6.0f &&
            m.x <= mx + mw && m.y <= my + mh;

        if (down && !g_imgWasDown && overHandle && !overImg) {
            g_resizing = true;
            g_resizeStartX = m.x;
            g_resizeStartY = m.y;
            g_resizeStartW = mw;
            g_resizeStartH = mh;
        }
        if (g_resizing && down) {
            float nw = g_resizeStartW + (m.x - g_resizeStartX);
            float nh = g_resizeStartH + (m.y - g_resizeStartY);
            if (nw < 300.0f) nw = 300.0f;
            if (nh < 220.0f) nh = 220.0f;
            if (nw > W - 12.0f) nw = W - 12.0f;
            if (nh > H - 12.0f) nh = H - 12.0f;
            p.menuW = nw;
            p.menuH = nh;
            if (p.menuX >= 0.0f && p.menuX + nw > W) p.menuX = W - nw;
            if (p.menuY >= 0.0f && p.menuY + nh > H) p.menuY = H - nh;
        }
        if (g_resizing && !down) {
            g_resizing = false;
            ui::save();
        }

        g_imgWasDown = down;
        g_ballWasDown = false;
    }

    g_openT = Approach(g_openT, g_menuOpen ? 1.0f : 0.0f, 14.0f, dt);
    g_ballT = Approach(g_ballT, g_menuOpen ? 0.0f : 1.0f, 16.0f, dt);
    StepAnim(g_menuOpen, dt);
    if (g_openT < 0.0015f) g_openT = (g_menuOpen ? 0.0015f : 0.0f);
    if (g_ballT < 0.0015f) g_ballT = (g_menuOpen ? 0.0f : 0.0015f);
}

}  // namespace

// ============================================================================
// 对外接口（渲染层每帧调一次）
// ============================================================================
namespace ui_layer {

static uint32_t g_ballTexture = 0;

void SetBallTexture(uint32_t tex) { g_ballTexture = tex; }
uint32_t BallTexture() { return g_ballTexture; }

void Frame(float W, float H, float dt) {
    g_lastDt = dt;
    HandleInteraction(W, H, dt);
    DrawMenu(W, H, dt);
    DrawBall(W, H);
}

bool MenuOpen() { return g_menuOpen; }

void ToggleMenu(bool open) {
    if (g_menuOpen == open) return;
    g_menuOpen = open;
    if (!open) ui::save();
}

}  // namespace ui_layer
