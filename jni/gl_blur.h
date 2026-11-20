#pragma once
#include <GLES3/gl3.h>
#include <android/log.h>
#include <cstdio>

// ============================================================================
// 背景高斯模糊
// ============================================================================
//
// 目标：菜单背后的游戏画面模糊，像 iOS 的毛玻璃。
//
// 做法是真正的两趟可分离高斯卷积，不是拿一堆半透明矩形糊弄：
//
//   1 把当前默认 framebuffer（游戏刚画完那一帧）抓进全分辨率纹理
//   2 横向 9-tap 高斯，同时降采样到 1/2 分辨率   -> texA
//   3 纵向 9-tap 高斯，仍在 1/2 分辨率            -> texB
//   4 texB 交给 ImGui 的 AddImage 放大铺回去
//      （GL_LINEAR 上采样本身就是一次平滑，等于免费多了半档模糊）
//
// 为什么降采样到 1/2：全分辨率做 9+9 次采样在中端机上是 2~3ms，1/2 只要
// 1/4 的开销，而视觉上没有区别 —— 高斯本来就是低通，先降采样不损失有效信息。
//
// 【失败即降级】
// 着色器编译、FBO 完整性、纹理分配任何一步失败，都把 g_failed 置位，
// 之后永久不再尝试，hook.cpp 那边会退回纯半透明面板。
// 宁可没有模糊，也不能因为模糊把整个 UI 干掉 —— 上一版已经因为
// 一个链接期问题让 UI 整个消失过一次了。

namespace blur {

inline GLuint g_texFull = 0, g_texA = 0, g_texB = 0;
inline GLuint g_fboFull = 0, g_fboA = 0, g_fboB = 0;
inline GLuint g_prog = 0, g_vao = 0;
inline GLint  g_uTex = -1, g_uTexel = -1, g_uDir = -1, g_uSpread = -1, g_uThreshold = -1;
inline float  g_spread = 1.0f;   // 模糊强度：采样步长倍数，1.0 为基准
inline float  g_threshold = 0.0f; // 明度阈值，0=全部模糊
inline int    g_w = 0, g_h = 0, g_hw = 0, g_hh = 0;
inline bool   g_failed = false;
// ★ 崩溃过一次就永久禁用（Android 16 兼容）
inline bool   g_disabled = false;
inline int    g_crashCount = 0;
inline bool   g_ok = false;

inline const char* kVS =
    "#version 300 es\n"
    "out vec2 v_uv;\n"
    "void main(){\n"
    "  vec2 p = vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2));\n"
    "  v_uv = p;\n"
    "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
    "}\n";

inline const char* kFS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel;\n"
    "uniform vec2 u_dir;\n"
    "uniform float u_spread;\n"          // 采样步长倍数 = 模糊强度
    "uniform float u_threshold;\n"       // 明度阈值：低于此值保留原样（不糊）
    "void main(){\n"
    "  vec4 src = texture(u_tex, v_uv);\n"
    "  vec2 off = u_texel * u_dir * u_spread;\n"
    "  vec4 s = src * 0.2270270270;\n"
    "  s += (texture(u_tex, v_uv+off)      + texture(u_tex, v_uv-off))      * 0.1945945946;\n"
    "  s += (texture(u_tex, v_uv+off*2.0)  + texture(u_tex, v_uv-off*2.0))  * 0.1216216216;\n"
    "  s += (texture(u_tex, v_uv+off*3.0)  + texture(u_tex, v_uv-off*3.0))  * 0.0540540541;\n"
    "  s += (texture(u_tex, v_uv+off*4.0)  + texture(u_tex, v_uv-off*4.0))  * 0.0162162162;\n"
    // 阈值：亮度 >= u_threshold 的像素保留原图（清晰），其余照常模糊
    "  float lum = dot(src.rgb, vec3(0.299, 0.587, 0.114));\n"
    "  float keep = (u_threshold > 0.001) ? step(u_threshold, lum) : 0.0;\n"
    "  fragColor = mix(s, src, keep);\n"
    "}\n";

inline GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    if (!s) return 0;
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        __android_log_print(ANDROID_LOG_ERROR, "FlcyUI",
                            "blur: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

inline void destroy() {
    if (g_prog)   { glDeleteProgram(g_prog);   g_prog = 0; }
    if (g_vao)    { glDeleteVertexArrays(1, &g_vao); g_vao = 0; }
    GLuint tex[3] = { g_texFull, g_texA, g_texB };
    GLuint fbo[3] = { g_fboFull, g_fboA, g_fboB };
    for (int i = 0; i < 3; ++i) {
        if (tex[i]) { glDeleteTextures(1, &tex[i]); tex[i] = 0; }
        if (fbo[i]) { glDeleteFramebuffers(1, &fbo[i]); fbo[i] = 0; }
    }
    g_texFull = g_texA = g_texB = 0;
    g_fboFull = g_fboA = g_fboB = 0;
    g_w = g_h = g_hw = g_hh = 0;
    g_ok = false;
}

inline GLuint makeTex(int w, int h) {
    GLuint t = 0;
    glGenTextures(1, &t);
    if (!t) return 0;
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return t;
}

inline GLuint makeFbo(GLuint tex) {
    GLuint f = 0;
    glGenFramebuffers(1, &f);
    if (!f) return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        __android_log_print(ANDROID_LOG_ERROR, "FlcyUI",
                            "blur: fbo incomplete 0x%x", (unsigned)st);
        glDeleteFramebuffers(1, &f);
        return 0;
    }
    return f;
}

inline bool ensure(int w, int h) {
    if (g_failed) return false;
    if (g_ok && w == g_w && h == g_h) return true;

    destroy();

    if (w < 8 || h < 8) { g_failed = true; return false; }

    g_w = w; g_h = h;
    g_hw = (w + 1) / 2;
    g_hh = (h + 1) / 2;
    if (g_hw < 4) g_hw = 4;
    if (g_hh < 4) g_hh = 4;

    GLuint vs = compile(GL_VERTEX_SHADER, kVS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        g_failed = true;
        return false;
    }

    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        GLsizei n = 0;
        glGetProgramInfoLog(g_prog, sizeof(log), &n, log);
        __android_log_print(ANDROID_LOG_ERROR, "FlcyUI",
                            "blur: link failed: %s", log);
        g_failed = true;
        return false;
    }

    g_uTex   = glGetUniformLocation(g_prog, "u_tex");
    g_uTexel = glGetUniformLocation(g_prog, "u_texel");
    g_uDir   = glGetUniformLocation(g_prog, "u_dir");
    g_uSpread= glGetUniformLocation(g_prog, "u_spread");
    g_uThreshold = glGetUniformLocation(g_prog, "u_threshold");

    glGenVertexArrays(1, &g_vao);

    g_texFull = makeTex(g_w, g_h);
    g_texA    = makeTex(g_hw, g_hh);
    g_texB    = makeTex(g_hw, g_hh);
    if (!g_texFull || !g_texA || !g_texB) { g_failed = true; return false; }

    g_fboFull = makeFbo(g_texFull);
    g_fboA    = makeFbo(g_texA);
    g_fboB    = makeFbo(g_texB);
    if (!g_fboFull || !g_fboA || !g_fboB) {
        __android_log_print(ANDROID_LOG_ERROR, "FlcyUI",
                            "blur: FBO 创建失败 full=%u A=%u B=%u",
                            g_fboFull, g_fboA, g_fboB);
        g_failed = true;
        return false;
    }

    // ★ 逐个检查 FBO 完整性 —— 不完整就整体放弃
    {
        const GLuint fbos[3] = { g_fboFull, g_fboA, g_fboB };
        const char* names[3] = { "full", "A", "B" };
        for (int i = 0; i < 3; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
            const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) {
                __android_log_print(ANDROID_LOG_ERROR, "FlcyUI",
                                    "blur: FBO %s 不完整 status=0x%x", names[i], st);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g_failed = true;
                return false;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    g_ok = true;
    __android_log_print(ANDROID_LOG_INFO, "FlcyUI",
                        "blur: ready %dx%d -> %dx%d", g_w, g_h, g_hw, g_hh);
    return true;
}

// 抓当前默认帧缓冲的**指定矩形**并做两趟高斯，返回模糊纹理（失败返回 0）。
// 内部完整保存/恢复 GL 状态，调用方无需关心。
//
// (rx, ry, rw, rh) 是「左上原点」像素矩形。GL 的 glCopyTexSubImage2D 用的是
// 左下原点，所以这里负责翻转 Y，调用方按 ImGui 习惯传参即可。
inline GLuint runRegion(int rx, int ry, int rw, int rh, int screenH) {
    if (rw < 8 || rh < 8) return 0;

    // ★ 全局降级闸门：一旦崩过就永久不再尝试
    if (g_disabled) return 0;

    if (!ensure(rw, rh)) return 0;

    // ---- 保存现场 ----
    GLint lastFbo = 0, lastViewport[4] = {0,0,0,0}, lastProg = 0;
    GLint lastTex = 0, lastActive = 0, lastVao = 0, lastBuf = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &lastFbo);
    glGetIntegerv(GL_VIEWPORT, lastViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &lastProg);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &lastActive);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastBuf);
    const GLboolean lastBlend   = glIsEnabled(GL_BLEND);
    const GLboolean lastDepth   = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean lastScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean lastCull    = glIsEnabled(GL_CULL_FACE);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);

    // ---- 1 抓帧（指定矩形，Y 从左上翻到左下）----
    int glY = screenH - ry - rh;
    if (glY < 0) glY = 0;
    glBindTexture(GL_TEXTURE_2D, g_texFull);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rx, glY, rw, rh);

    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(g_uTex, 0);
    glUniform1f(g_uSpread, g_spread < 0.05f ? 0.05f : g_spread);
    glUniform1f(g_uThreshold, g_threshold < 0.0f ? 0.0f : (g_threshold > 1.0f ? 1.0f : g_threshold));
    glViewport(0, 0, g_hw, g_hh);

    // ---- 2 横向：原尺寸 -> 半尺寸 ----
    glBindFramebuffer(GL_FRAMEBUFFER, g_fboA);
    glBindTexture(GL_TEXTURE_2D, g_texFull);
    glUniform2f(g_uTexel, 1.0f / (float)g_w, 1.0f / (float)g_h);
    glUniform2f(g_uDir, 1.0f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ---- 3 纵向 ----
    glBindFramebuffer(GL_FRAMEBUFFER, g_fboB);
    glBindTexture(GL_TEXTURE_2D, g_texA);
    glUniform2f(g_uTexel, 1.0f / (float)g_hw, 1.0f / (float)g_hh);
    glUniform2f(g_uDir, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ---- 恢复现场 ----
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)lastFbo);
    glViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
    glUseProgram((GLuint)lastProg);
    glActiveTexture((GLenum)lastActive);
    glBindTexture(GL_TEXTURE_2D, (GLuint)lastTex);
    glBindVertexArray((GLuint)lastVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)lastBuf);
    if (lastBlend)   glEnable(GL_BLEND);   else glDisable(GL_BLEND);
    if (lastDepth)   glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (lastScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (lastCull)    glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);

    return g_texB;
}

// 全屏版本，保留（其它地方可能还要）
inline GLuint run(int w, int h) {
    return runRegion(0, 0, w, h, h);
}

}  // namespace blur
