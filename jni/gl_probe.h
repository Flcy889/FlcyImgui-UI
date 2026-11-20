#pragma once
#include <GLES3/gl3.h>

// ============================================================================
// GL / EGL 现场探针
// ============================================================================
//
// 触摸问题修了 7 轮不收敛，根因之一是**从来没有拿到过"这一帧到底画在多大
// 的缓冲上、用的是哪个 FBO"这个事实**，只能靠截图比例反推。
//
// 这一层在 eglSwapBuffers 里、ImGui 渲染之前，同时读出三件互相独立的事实：
//
//   1. EGL surface 尺寸       —— eglQuerySurface
//   2. 当前 GL viewport 矩形  —— glGetIntegerv(GL_VIEWPORT)
//   3. 当前绑定的 FBO         —— glGetIntegerv(GL_FRAMEBUFFER_BINDING)
//
// 判据直接可读：
//
//   FBO != 0
//       Unity 把画面画进自己的离屏目标，我们再叠一层，最后由它 blit 上屏。
//       "看到的位置"和"ImGui 认为的位置"天生不在同一空间 —— 正是
//       「视觉中心 ≠ 命中区域、越靠边差越多」的教科书成因。
//
//   viewport != surface
//       投影被缩放，同样产生比例型错位。
//
//   两者都正常但命中仍偏
//       那才是纯粹的触摸坐标映射问题，用 touch_calib 的四点标定一次量出来。

namespace glprobe {

struct FrameInfo {
    int  surfaceW = 0, surfaceH = 0;                     // EGL surface
    int  vpX = 0, vpY = 0, vpW = 0, vpH = 0;             // GL_VIEWPORT
    int  fbo = -1;                                       // GL_FRAMEBUFFER_BINDING
    bool valid = false;
};

inline void query(FrameInfo& info) {
    info.vpX = info.vpY = info.vpW = info.vpH = 0;
    info.fbo = -1;

    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    info.vpX = vp[0]; info.vpY = vp[1];
    info.vpW = vp[2]; info.vpH = vp[3];

    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    info.fbo = (int)fbo;

    info.valid = (info.vpW > 0 && info.vpH > 0);
}

}  // namespace glprobe
