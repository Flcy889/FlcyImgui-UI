// ============================================================================
// ui_overlay.cpp —— 渲染接管层（EGL swap hook + ImGui 帧循环）
// ============================================================================
// 职责只有一件事：在宿主应用（任何 OpenGL ES 应用）每帧交换缓冲之前，
// 把 ImGui 画上去。具体画什么由 ui_menu.cpp 决定。
//
// 这里保留的是原项目的运行时骨架：
//   · eglSwapBuffers / WithDamageKHR / 2KHR 三路 hook（Android 12+ 只走后者）
//   · ANativeWindow 尺寸 hook + eglQuerySurface 四路兜底取尺寸
//   · libEGL 句柄的多路获取（Android 14+ dlopen 可能被 namespace 拦）
//   · hook 失败后台重试（最多 60 秒）
//   · 段级 SIGSEGV 保护 + 分级降级（崩了降一级而不是整进程死）
//   · MotionEvent 触摸转发
//
// 已移除：il2cpp 方法 hook、OnGUI 回调、卡密/网络验证、游戏功能调度。
// 也就是说这一层不认识任何游戏，换成别的 Unity / 原生 GL 应用一样能挂。
// ============================================================================

#include <setjmp.h>
#include <signal.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <imgui.h>
#include <imgui_impl_android.h>
#include <imgui_impl_opengl3.h>

#include <Dobby/dobby.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "ball_texture.h"
#include "flcy_log.h"
#include "gl_blur.h"
#include "gl_probe.h"
#include "repair_font.h"
#include "touch_calib.h"
#include "touch_display.h"
#include "touch_transform.h"
#include "ui/ui_layer.h"
#include "ui_prefs.h"

#define FLCY_LOG(...)      flcylog::Write("ImGuiHook", __VA_ARGS__)
#define FLCY_LOG_T(t, ...) flcylog::Write(t, __VA_ARGS__)

// ============================================================================
// 段级崩溃保护
// ============================================================================
// 用法：FLCY_SEGMENT("段名", { 代码 })
// 崩了就跳过这一段、打日志、继续下一段 —— 不会杀进程。
// 真机上"某个 GL 查询在特定驱动上必崩"这种坑，靠这个把整条绘制链保住。
static sigjmp_buf g_segJmp;
static volatile sig_atomic_t g_segSegv = 0;
static const char* g_segName = "?";

static void SegSegvHandler(int) {
    g_segSegv = 1;
    siglongjmp(g_segJmp, 1);
}

#define FLCY_SEGMENT(name, code)                                            \
    ([&]() -> bool {                                                        \
        struct sigaction _sa, _old;                                         \
        memset(&_sa, 0, sizeof _sa);                                        \
        _sa.sa_handler = SegSegvHandler;                                    \
        sigemptyset(&_sa.sa_mask);                                          \
        _sa.sa_flags = SA_NODEFER;                                          \
        sigaction(SIGSEGV, &_sa, &_old);                                    \
        g_segSegv = 0;                                                      \
        g_segName = (name);                                                 \
        bool _ok = false;                                                   \
        if (sigsetjmp(g_segJmp, 1) == 0) {                                  \
            code;                                                           \
            _ok = !g_segSegv;                                               \
        }                                                                   \
        sigaction(SIGSEGV, &_old, nullptr);                                 \
        if (!_ok) {                                                         \
            FLCY_LOG_T("ImGuiHook", "★★ 段 [%s] 崩溃，已跳过", (name));     \
        }                                                                   \
        return _ok;                                                         \
    }())

// ============================================================================
// 分级降级
// ============================================================================
//   L0  全部（模糊 + 菜单 + 悬浮球）
//   L1  无模糊（模糊在个别 Adreno 驱动上会崩）
//   L2  仅菜单（不再做 GL 探针）
// 每级独立 SIGSEGV 保护，崩了自动降一级并写日志。状态不持久化，重启即复位。
static int g_degradeLevel = 0;

static const char* DegradeName(int lv) {
    switch (lv) {
        case 0: return "L0 全功能";
        case 1: return "L1 无模糊";
        case 2: return "L2 仅菜单";
        default: return "L?";
    }
}

// ============================================================================
// 全局状态
// ============================================================================
static volatile bool g_initialized = false;

static int g_surfaceWidth = 0, g_surfaceHeight = 0;
static int g_windowWidth = 0, g_windowHeight = 0;
static ANativeWindow* g_nativeWindow = nullptr;

static glprobe::FrameInfo g_frame;
static touch::DisplayInfo g_display;

typedef EGLBoolean (*T_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef void (*MotionEvent_copyFrom)(void*, const void*, bool);
typedef int32_t (*ANativeWindow_GetSize)(ANativeWindow*);

static T_eglSwapBuffers orig_eglSwapBuffers = nullptr;

// Android 12+ / 13+ 的替代入口（不 hook 这些会接不到帧）
typedef EGLBoolean (*T_eglSwapBuffersWithDamageKHR)(EGLDisplay, EGLSurface,
                                                    const EGLint*, EGLint);
static T_eglSwapBuffersWithDamageKHR orig_eglSwapBuffersWithDamageKHR = nullptr;

typedef EGLBoolean (*T_eglSwapBuffers2KHR)(EGLDisplay, EGLSurface,
                                           const EGLint*, EGLint,
                                           const EGLint*, EGLint);
static T_eglSwapBuffers2KHR orig_eglSwapBuffers2KHR = nullptr;

static MotionEvent_copyFrom orig_MotionEventCopy = nullptr;
static ANativeWindow_GetSize orig_GetWidth = nullptr;
static ANativeWindow_GetSize orig_GetHeight = nullptr;

EGLBoolean hook_eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean hook_eglSwapBuffersWithDamageKHR(EGLDisplay, EGLSurface,
                                            const EGLint*, EGLint);
EGLBoolean hook_eglSwapBuffers2KHR(EGLDisplay, EGLSurface,
                                   const EGLint*, EGLint,
                                   const EGLint*, EGLint);

// ============================================================================
// 工具
// ============================================================================
bool isLibraryLoaded(const char* name) {
    if (!name) return false;
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, name)) { found = true; break; }
    }
    fclose(fp);
    return found;
}

static uint32_t CreateBallTexture() {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, BALL_TEX_W, BALL_TEX_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, g_ballTexPixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return (uint32_t)tex;
}

// ============================================================================
// 原生窗口 / 触摸转发
// ============================================================================
// 触摸事件原样交给宿主，另外复制一份给 ImGui —— 不影响宿主原有输入逻辑。
void hook_MotionEventCopy(void* thiz, const void* other, bool keepHistory) {
    orig_MotionEventCopy(thiz, other, keepHistory);
    if (g_initialized) {
        ImGui_ImplAndroid_HandleTouchEvent(
            reinterpret_cast<AInputEvent*>(thiz), 0, 0, 0, 0, 0);
    }
}

int32_t hook_GetWidth(ANativeWindow* window) {
    g_nativeWindow = window;
    g_windowWidth = orig_GetWidth(window);
    return g_windowWidth;
}

int32_t hook_GetHeight(ANativeWindow* window) {
    g_windowHeight = orig_GetHeight(window);
    return g_windowHeight;
}

// ============================================================================
// EGL 交换缓冲 Hook
// ============================================================================
// WithDamage / 2KHR 是 Android 12+ / 13+ 的替代 swap 入口。
// 部分 Unity 只走这两个，不 hook 会完全接不到帧。
// 绘制逻辑复用 hook_eglSwapBuffers。
EGLBoolean hook_eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface,
                                             const EGLint* rects, EGLint n_rects) {
    static thread_local bool s_in = false;
    if (s_in) {
        return orig_eglSwapBuffersWithDamageKHR
             ? orig_eglSwapBuffersWithDamageKHR(dpy, surface, rects, n_rects)
             : EGL_TRUE;
    }
    s_in = true;
    EGLBoolean r = hook_eglSwapBuffers(dpy, surface);
    s_in = false;
    return r;
}

EGLBoolean hook_eglSwapBuffers2KHR(EGLDisplay dpy, EGLSurface surface,
                                    const EGLint* rects, EGLint n_rects,
                                    const EGLint* attribs, EGLint n_attribs) {
    static thread_local bool s_in = false;
    if (s_in) {
        return orig_eglSwapBuffers2KHR
             ? orig_eglSwapBuffers2KHR(dpy, surface, rects, n_rects, attribs, n_attribs)
             : EGL_TRUE;
    }
    s_in = true;
    EGLBoolean r = hook_eglSwapBuffers(dpy, surface);
    s_in = false;
    return r;
}

// ============================================================================
// 可靠地取 surface 尺寸（Android 16 兜底）
// ============================================================================
// eglQuerySurface 在某些驱动 / swap 时机下会失败或返回 0。
// 这里多路尝试，拿到就缓存。
static int  g_qsFailCount = 0;
static bool g_sizeLocked  = false;

static bool QuerySurfaceSize(EGLDisplay dpy, EGLSurface surface,
                             int* outW, int* outH) {
    if (!outW || !outH) return false;

    // ---- ① 标准路径 ----
    EGLint w = 0, h = 0;
    const EGLBoolean okW = eglQuerySurface(dpy, surface, EGL_WIDTH,  &w);
    const EGLBoolean okH = eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);

    if (okW && okH && w > 0 && h > 0) {
        *outW = (int)w;
        *outH = (int)h;
        return true;
    }

    // ---- ② ANativeWindow 路径 ----
    if (g_nativeWindow && orig_GetWidth && orig_GetHeight) {
        const int nw = orig_GetWidth(g_nativeWindow);
        const int nh = orig_GetHeight(g_nativeWindow);
        if (nw > 0 && nh > 0) {
            *outW = nw;
            *outH = nh;
            return true;
        }
    }

    // ---- ③ 之前 hook 到的窗口尺寸 ----
    if (g_windowWidth > 0 && g_windowHeight > 0) {
        *outW = g_windowWidth;
        *outH = g_windowHeight;
        return true;
    }

    // ---- ④ 屏幕尺寸（最兜底，可能和 surface 不等但能跑） ----
    if (!g_display.detected) g_display = touch::queryDisplayInfo();
    if (g_display.width > 0 && g_display.height > 0) {
        const int rot = g_display.rotation;
        if (rot == 1 || rot == 3) {
            *outW = g_display.height;
            *outH = g_display.width;
        } else {
            *outW = g_display.width;
            *outH = g_display.height;
        }
        return true;
    }

    return false;
}

// ============================================================================
// 每帧绘制
// ============================================================================
static void InitImGuiOnce() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig font_cfg;
    font_cfg.FontDataOwnedByAtlas = false;
    io.FontDefault = io.Fonts->AddFontFromMemoryTTF(
        (void*)repair_font, sizeof(repair_font), 25.0f, &font_cfg,
        io.Fonts->GetGlyphRangesChineseFull());
    if (!io.FontDefault) io.FontDefault = io.Fonts->AddFontDefault();

    ImGui_ImplOpenGL3_Init("#version 300 es");
    ui::load();
    touch::calib::load();

    ui_layer::SetBallTexture(CreateBallTexture());
    g_initialized = true;
    FLCY_LOG_T("ImGuiInit", "ImGui 初始化完成 surface=%dx%d",
               g_surfaceWidth, g_surfaceHeight);
}

static void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 22.0f;
    s.ChildRounding  = 10.0f;
    s.FrameRounding  = 8.0f;
    s.GrabRounding   = 8.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize  = 0.0f;
    s.ItemSpacing    = ImVec2(8.0f, 8.0f);
    s.FramePadding   = ImVec2(10.0f, 6.0f);
    s.ScrollbarSize  = 0.0f;

    const bool dark = ui::prefs.forceBlack || ui::prefs.darkTheme;
    const ImVec4 tx = ImGui::ColorConvertU32ToFloat4(dark
        ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
    const ImVec4 ac = ImGui::ColorConvertU32ToFloat4(
        ui::hsv(ui::prefs.accentHue, ui::prefs.accentSat, 0.94f, 1.0f));
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]          = tx;
    c[ImGuiCol_WindowBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg]       = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Button]        = ImVec4(ac.x, ac.y, ac.z, 0.18f);
    c[ImGuiCol_ButtonHovered] = ImVec4(ac.x, ac.y, ac.z, 0.34f);
    c[ImGuiCol_ButtonActive]  = ImVec4(ac.x, ac.y, ac.z, 0.50f);

    if (ui::prefs.uiScale > 0.01f) {
        ImGui::GetIO().FontGlobalScale = ui::prefs.uiScale;
        s.ScaleAllSizes(ui::prefs.uiScale);
    } else {
        ImGui::GetIO().FontGlobalScale = 1.0f;
    }
}

static float NowSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (float)ts.tv_sec + (float)ts.tv_nsec / 1e9f;
}

static void DrawFrame(float dt) {
    const int lw = g_surfaceWidth;
    const int lh = g_surfaceHeight;
    ImGuiIO& io = ImGui::GetIO();

    // ---- 屏幕 / 旋转信息（失败不锁缓存，下一帧再试）----
    if (g_degradeLevel < 2) {
        FLCY_SEGMENT("glprobe", { glprobe::query(g_frame); });
    }
    FLCY_SEGMENT("queryDisplay", {
        if (!g_display.detected) g_display = touch::queryDisplayInfo();
    });

    touch::calib::surfaceW = (float)lw;
    touch::calib::surfaceH = (float)lh;
    touch::calib::windowW  = (float)g_windowWidth;
    touch::calib::windowH  = (float)g_windowHeight;
    touch::calib::displayW = (float)g_display.width;
    touch::calib::displayH = (float)g_display.height;
    touch::calib::rotation = g_display.rotation;
    touch::calib::xform    = touch::rotationToTransform(g_display.rotation);

    io.DisplaySize = ImVec2((float)lw, (float)lh);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(lw, lh, ImVec2(1.0f, 1.0f));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    ApplyStyle();

    if (!FLCY_SEGMENT("ImGuiNewFrame", { ImGui::NewFrame(); })) {
        return;
    }

    ui_layer::Frame((float)lw, (float)lh, dt);

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static float g_lastTime = 0.0f;

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    // ---- 取尺寸（多路兜底）----
    {
        int w = 0, h = 0;
        const bool got = QuerySurfaceSize(dpy, surface, &w, &h);
        if (got) {
            g_surfaceWidth  = w;
            g_surfaceHeight = h;
        }

        static int s_dbg = 0;
        if (s_dbg < 5 || (s_dbg % 300) == 0) {
            FLCY_LOG("swap#%d query=%s size=%dx%d win=%dx%d disp=%dx%d rot=%d init=%d",
                     s_dbg, got ? "OK" : "FAIL",
                     g_surfaceWidth, g_surfaceHeight,
                     g_windowWidth, g_windowHeight,
                     g_display.width, g_display.height, g_display.rotation,
                     g_initialized ? 1 : 0);
        }
        ++s_dbg;
    }

    if (!g_initialized && g_surfaceWidth > 0 && g_surfaceHeight > 0) {
        InitImGuiOnce();
    }

    if (g_initialized) {
        const float t = NowSeconds();
        float dt = (g_lastTime > 0.0f) ? (t - g_lastTime) : (1.0f / 60.0f);
        g_lastTime = t;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.100f) dt = 0.100f;

        // ---- 模糊崩溃自动降级 ----
        if (blur::g_disabled && g_degradeLevel < 1) {
            g_degradeLevel = 1;
            FLCY_LOG_T("ImGuiHook", "★★ 模糊崩溃 -> 降级到 %s", DegradeName(g_degradeLevel));
        }

        DrawFrame(dt);

        static int s_diag = 0;
        if (s_diag < 5 || (s_diag % 300) == 0) {
            ++s_diag;
            FLCY_LOG("draw#%d surface=%dx%d vtx=%d level=%s",
                     s_diag, g_surfaceWidth, g_surfaceHeight,
                     ImGui::GetDrawData() ? ImGui::GetDrawData()->TotalVtxCount : 0,
                     DegradeName(g_degradeLevel));
        }
    }

    return orig_eglSwapBuffers(dpy, surface);
}

// ============================================================================
// 初始化
// ============================================================================
// 取 libEGL 句柄（Android 14+ dlopen 可能被 namespace 拦）
static void* GetEglHandle() {
    // dlopen 成功返回的句柄一定页对齐（低 12 位为 0），用它做合法性校验
    auto plausible = [](void* h) -> bool {
        return h && ((uintptr_t)h & 0xFFF) == 0;
    };

    // ① 直接 dlopen
    {
        void* h = dlopen("libEGL.so", RTLD_NOW);
        if (plausible(h)) return h;
        if (h) FLCY_LOG("dlopen(libEGL) 返回可疑值 %p，丢弃", h);
    }

    // ② RTLD_NOLOAD 拿已加载的
    {
        void* h = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (plausible(h)) return h;
    }

    // ③ 从 maps 取全路径（严格按列解析，避免切出野句柄）
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            if (!strstr(line, "libEGL.so")) continue;

            char* save = nullptr;
            char* tok = strtok_r(line, " \t\n", &save);
            int col = 0;
            char* pathStart = nullptr;
            while (tok) {
                ++col;
                if (col == 6) { pathStart = tok; break; }
                tok = strtok_r(nullptr, " \t\n", &save);
            }

            if (!pathStart || pathStart[0] != '/') continue;
            if (!strstr(pathStart, ".so")) continue;

            void* h = dlopen(pathStart, RTLD_NOW);
            if (plausible(h)) {
                FLCY_LOG("从 maps 加载 libEGL: %s", pathStart);
                fclose(fp);
                return h;
            }
            FLCY_LOG("maps 路径 dlopen 失败: %s", pathStart);
            break;
        }
        fclose(fp);
    }

    // ④ 不需要句柄也能工作，返回 nullptr，上层会走 RTLD_DEFAULT
    return nullptr;
}

// Android 16 环境探测
// __system_property_get 在 libc 里，不需要读 build.prop（Android 7+ 已禁读）
extern "C" int __system_property_get(const char* name, char* value);

static void ProbeEnv() {
    FLCY_LOG_T("ImGuiInit", "===== 启动横幅 =====");
    FLCY_LOG_T("ImGuiInit", "build = " __DATE__ " " __TIME__);

    char proc[128] = {0};
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (f) {
        size_t n = fread(proc, 1, sizeof(proc) - 1, f);
        if (n > 0) proc[n] = 0;
        fclose(f);
    }
    FLCY_LOG_T("ImGuiInit", "进程 = %s", proc[0] ? proc : "?");

    char rel[92] = {0}, sdk[92] = {0}, model[92] = {0}, brand[92] = {0}, abi[92] = {0};
    __system_property_get("ro.build.version.release", rel);
    __system_property_get("ro.build.version.sdk", sdk);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.product.brand", brand);
    __system_property_get("ro.product.cpu.abi", abi);

    FLCY_LOG_T("ImGuiInit", "Android = %s (SDK %s)", rel[0] ? rel : "?", sdk[0] ? sdk : "?");
    FLCY_LOG_T("ImGuiInit", "机型 = %s %s", brand[0] ? brand : "?", model[0] ? model : "?");
    FLCY_LOG_T("ImGuiInit", "ABI = %s", abi[0] ? abi : "?");
    FLCY_LOG_T("ImGuiInit", "页面大小 = %ld KB", sysconf(_SC_PAGESIZE) / 1024);

    const char* libs[] = {
        "libEGL.so", "libGLESv2.so", "libGLESv3.so",
        "libandroid.so", "libgui.so", "libinput.so",
        "libunity.so", "libmain.so", "libvulkan.so",
        nullptr
    };
    for (int i = 0; libs[i]; ++i) {
        FLCY_LOG_T("ImGuiInit", "  库 %-16s %s", libs[i],
                   isLibraryLoaded(libs[i]) ? "有" : "无");
    }
    FLCY_LOG_T("ImGuiInit", "====================");
}

static bool WaitLib(const char* name, int timeoutMs) {
    const int step = 50;
    for (int waited = 0; waited < timeoutMs; waited += step) {
        if (isLibraryLoaded(name)) return true;
        usleep(step * 1000);
    }
    return isLibraryLoaded(name);
}

// ============================================================================
// 延迟 hook（修注入成功率低）
// ============================================================================
// Init() 在进程启动极早期执行，那时 libEGL 可能还没加载，
// dlsym 拿不到符号，hook 直接失败。没挂上的交给后台线程轮询重试，
// 直到成功或超时。重试只碰 hook 安装，不碰 GL 状态，安全。
static volatile int g_hookedSwap = 0;
static volatile int g_hookedDmg  = 0;
static volatile int g_hookedKhr2 = 0;
static volatile int g_retryStop  = 0;
static volatile int g_retryDone  = 0;

static bool TryHookOneSwap(const char* name, void* handler, void** orig,
                           volatile int* flag) {
    if (*flag) return true;

    void* sym = dlsym(RTLD_DEFAULT, name);
    if (!sym) {
        void* h = GetEglHandle();
        if (h) sym = dlsym(h, name);
    }
    if (!sym) return false;

    const int rc = DobbyHook(sym, handler, orig);
    if (rc == 0) {
        *flag = 1;
        FLCY_LOG("重试成功: %s @ %p", name, sym);
        return true;
    }
    FLCY_LOG("重试: %s hook 失败 rc=%d", name, rc);
    return false;
}

static bool TryHookAllSwap() {
    bool any = false;
    if (TryHookOneSwap("eglSwapBuffers", (void*)hook_eglSwapBuffers,
                       (void**)&orig_eglSwapBuffers, &g_hookedSwap))
        any = true;
    if (TryHookOneSwap("eglSwapBuffersWithDamageKHR",
                       (void*)hook_eglSwapBuffersWithDamageKHR,
                       (void**)&orig_eglSwapBuffersWithDamageKHR, &g_hookedDmg))
        any = true;
    if (TryHookOneSwap("eglSwapBuffers2KHR", (void*)hook_eglSwapBuffers2KHR,
                       (void**)&orig_eglSwapBuffers2KHR, &g_hookedKhr2))
        any = true;
    return any;
}

static void* SwapRetryThread(void*) {
    const int maxWaitMs = 60000;
    const int stepMs    = 200;
    int waited = 0;
    int tried = 0;

    FLCY_LOG("重试线程启动");
    while (!g_retryStop && waited < maxWaitMs) {
        ++tried;
        if (TryHookAllSwap()) {
            g_retryDone = 1;
            FLCY_LOG("重试线程: 挂上了（第 %d 次，%.1f 秒）", tried, waited / 1000.0f);
            usleep(500 * 1000);
            if (g_hookedSwap && g_hookedDmg) break;
        }
        usleep(stepMs * 1000);
        waited += stepMs;
    }
    if (!g_retryDone) {
        FLCY_LOG("★★ 重试线程超时 %d 秒，从未挂上 swap 入口", waited / 1000);
    }
    return nullptr;
}

static void StartSwapRetry() {
    static bool started = false;
    if (started) return;
    started = true;

    pthread_t th;
    if (pthread_create(&th, nullptr, SwapRetryThread, nullptr) == 0) {
        pthread_detach(th);
        FLCY_LOG("已启动 swap 重试线程");
    } else {
        FLCY_LOG("★★ 重试线程创建失败");
    }
}

// ============================================================================
// 入口
// ============================================================================
__attribute__((constructor)) void Init() {
    ProbeEnv();

    // ---- ① 窗口尺寸 hook ----
    const bool hasAndroid = WaitLib("libandroid.so", 5000);
    FLCY_LOG_T("ImGuiInit", "等待 libandroid.so -> %s", hasAndroid ? "OK" : "超时");

    void* androidLib = hasAndroid ? dlopen("libandroid.so", RTLD_NOW) : nullptr;

    int r1 = -1, r2 = -1;
    {
        void* sW = nullptr;
        void* sH = nullptr;
        if (androidLib) {
            sW = dlsym(androidLib, "ANativeWindow_getWidth");
            sH = dlsym(androidLib, "ANativeWindow_getHeight");
        }
        if (!sW) sW = dlsym(RTLD_DEFAULT, "ANativeWindow_getWidth");
        if (!sH) sH = dlsym(RTLD_DEFAULT, "ANativeWindow_getHeight");

        FLCY_LOG("ANativeWindow_getWidth  符号 = %p", sW);
        FLCY_LOG("ANativeWindow_getHeight 符号 = %p", sH);

        if (sW) r1 = DobbyHook(sW, (void*)hook_GetWidth,  (void**)&orig_GetWidth);
        if (sH) r2 = DobbyHook(sH, (void*)hook_GetHeight, (void**)&orig_GetHeight);
        FLCY_LOG("窗口尺寸 hook: r1=%d r2=%d", r1, r2);
    }

    // ---- ② 触摸 hook ----
    int r3 = -1;
    {
        const char* syms[] = {
            "_ZN7android11MotionEvent8copyFromEPKS0_b",
            "_ZN7android11MotionEvent8copyFromEPKS0_",
            "_ZN7android11MotionEvent9copyFromEPKS0_b",
            nullptr
        };
        WaitLib("libinput.so", 5000);
        for (int i = 0; syms[i]; ++i) {
            void* p = DobbySymbolResolver("/system/lib64/libinput.so", syms[i]);
            if (!p) p = DobbySymbolResolver("libinput.so", syms[i]);
            FLCY_LOG("输入符号[%d] %s -> %p", i, syms[i], p);
            if (p) {
                r3 = DobbyHook(p, (void*)hook_MotionEventCopy, (void**)&orig_MotionEventCopy);
                FLCY_LOG("  输入 hook = %d", r3);
                break;
            }
        }
        if (r3 == -1) {
            void* p = dlsym(RTLD_DEFAULT, "_ZN7android11MotionEvent8copyFromEPKS0_b");
            if (p) r3 = DobbyHook(p, (void*)hook_MotionEventCopy, (void**)&orig_MotionEventCopy);
        }
        FLCY_LOG("输入 hook 最终 = %d", r3);
    }

    // ---- ③ EGL hook ----
    int r4 = -1, r4b = -1, r4c = -1;
    {
        WaitLib("libEGL.so", 8000);
        void* eglLib = GetEglHandle();
        FLCY_LOG("libEGL 句柄 = %p", eglLib);

        {
            void* pSwap = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
            if (!pSwap && eglLib) pSwap = dlsym(eglLib, "eglSwapBuffers");
            FLCY_LOG("eglSwapBuffers = %p", pSwap);
            if (pSwap) {
                r4 = DobbyHook(pSwap, (void*)hook_eglSwapBuffers,
                               (void**)&orig_eglSwapBuffers);
                if (r4 == 0) g_hookedSwap = 1;
            }
        }
        {
            void* pDmg = dlsym(RTLD_DEFAULT, "eglSwapBuffersWithDamageKHR");
            if (!pDmg && eglLib) pDmg = dlsym(eglLib, "eglSwapBuffersWithDamageKHR");
            FLCY_LOG("eglSwapBuffersWithDamageKHR = %p", pDmg);
            if (pDmg) {
                r4b = DobbyHook(pDmg, (void*)hook_eglSwapBuffersWithDamageKHR,
                                (void**)&orig_eglSwapBuffersWithDamageKHR);
                if (r4b == 0) g_hookedDmg = 1;
            }
        }
        {
            void* p2 = dlsym(RTLD_DEFAULT, "eglSwapBuffers2KHR");
            if (!p2 && eglLib) p2 = dlsym(eglLib, "eglSwapBuffers2KHR");
            FLCY_LOG("eglSwapBuffers2KHR = %p", p2);
            if (p2) {
                r4c = DobbyHook(p2, (void*)hook_eglSwapBuffers2KHR,
                                (void**)&orig_eglSwapBuffers2KHR);
                if (r4c == 0) g_hookedKhr2 = 1;
            }
        }

        const bool allOk = (g_hookedSwap && g_hookedDmg);
        if (!allOk) {
            FLCY_LOG("首次 hook 未完整 (swap=%d dmg=%d khr2=%d)，启动重试",
                     g_hookedSwap, g_hookedDmg, g_hookedKhr2);
            StartSwapRetry();
        } else {
            FLCY_LOG("首次 hook 全部成功");
        }
    }

    // ---- ④ 汇总 ----
    FLCY_LOG("===== 结果 getW=%d getH=%d input=%d swap=%d dmg=%d khr2=%d (0=成功) =====",
             r1, r2, r3, r4, r4b, r4c);
    FLCY_LOG("===== 已挂状态 swap=%d dmg=%d khr2=%d =====",
             g_hookedSwap, g_hookedDmg, g_hookedKhr2);

    const bool anySwap = (r4 == 0 || r4b == 0 || r4c == 0);
    if (!anySwap) {
        FLCY_LOG("★★ 所有 swap 入口都没挂上 —— 覆盖层不会显示");
        FLCY_LOG("★★ 可能原因：");
        FLCY_LOG("   1. 宿主用 Vulkan 渲染（不是 EGL）");
        FLCY_LOG("   2. 其他钩子框架先占了符号");
        FLCY_LOG("   3. Dobby 在这个 Android 版本失效");
    }
    FLCY_LOG_T("ImGuiInit", "Init 结束");
}
