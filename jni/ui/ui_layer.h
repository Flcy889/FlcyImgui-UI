#pragma once

// ============================================================================
// ui_layer.h —— 覆盖层对外接口
// ============================================================================
// 渲染层（EGL hook）每帧调一次 Frame()；
// 菜单层（ui_menu.cpp）从渲染层拿悬浮球贴图。
// 两层之间只有这一个头，互不依赖对方的内部状态。
// ============================================================================

#include <cstdint>

namespace ui_layer {

// 每帧调用一次：处理触摸交互 + 绘制菜单/悬浮球
void Frame(float W, float H, float dt);

// 菜单是否展开（宿主可据此决定要不要让游戏继续收到点击）
bool MenuOpen();
void ToggleMenu(bool open);

// 悬浮球贴图（GL 纹理 id，由渲染层创建后注入）
void     SetBallTexture(uint32_t tex);
uint32_t BallTexture();

}  // namespace ui_layer
