// ============================================================================
// flcy_log.h —— 落盘日志（UI 覆盖层版）
// ============================================================================
// 真机上 `adb logcat` 经常拿不到（没线、被过滤、进程重启），
// 而覆盖层是否挂上、每帧画的什么，必须能离线看。
// 本模块同时写 logcat 和文件。
//
// 路径策略：
//   ① /sdcard/imgui_ui_log.txt        外部存储，用户可见
//   ② /data/local/tmp/imgui_ui_log.txt 兜底（一定能写）
//
// 落盘 tag 白名单见 FileTags()；排查时把 kFileTagWhitelist 置 false 即可全量落盘。
// ============================================================================

#include <cstdio>

#ifndef IMGUI_UI_LOG_PATH
#define IMGUI_UI_LOG_PATH "/sdcard/imgui_ui_log.txt"
#endif
#ifndef IMGUI_UI_LOG_PATH_FALLBACK
#define IMGUI_UI_LOG_PATH_FALLBACK "/data/local/tmp/imgui_ui_log.txt"
#endif

#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctime>
#include <time.h>
#include <android/log.h>

namespace flcylog {

// ============================================================================
// 日志开关：只保留指定 tag 落盘
// ============================================================================
// 默认只落 UI 相关 tag，避免宿主运行时刷屏。
//
//   kFileTagWhitelist = true  → 只写 FileTags() 里的 tag（默认）
//   kFileTagWhitelist = false → 不过滤，所有 tag 都落盘（排查时用）
// ============================================================================
constexpr bool  kFileLogOn   = true;              // 落盘总开关
// 白名单：只这些 tag 落盘
inline const char* const* FileTags() {
    static const char* const t[] = {
        "ImGuiInit",
        "ImGuiHook",
        "ImGuiUI",
        "ImGuiMirror",
        nullptr
    };
    return t;
}
constexpr bool kFileTagWhitelist = true;   // false = 不过滤
constexpr bool  kLogcatOn    = true;              // logcat 仍然输出

// tag 是否允许落盘
inline bool TagAllowed(const char* tag) {
    if (!tag) return false;
    if (!kFileTagWhitelist) return true;
    const char* const* t = FileTags();
    for (int i = 0; t[i]; ++i) {
        if (strcmp(tag, t[i]) == 0) return true;
    }
    return false;
}



// 候选路径（可用编译宏覆盖）
inline const char* const* Paths() {
    static const char* const p[] = {
        IMGUI_UI_LOG_PATH,
        IMGUI_UI_LOG_PATH_FALLBACK,
        nullptr
    };
    return p;
}

// 首次调用时定位并缓存可写路径
inline const char* ResolvePath() {
    static char s_path[256] = "";
    static int  s_tried = 0;
    if (s_tried) return s_path[0] ? s_path : nullptr;
    s_tried = 1;

    const char* const* ps = Paths();
    for (int i = 0; ps[i]; ++i) {
        // 先确保父目录存在（外部目录一般游戏已建好，私有目录可能没有）
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", ps[i]);
        char* slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(dir, 0755);
        }

        int fd = open(ps[i], O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) continue;
        close(fd);
        snprintf(s_path, sizeof(s_path), "%s", ps[i]);
        __android_log_print(ANDROID_LOG_INFO, "FlcyLog", "日志文件: %s", s_path);
        return s_path;
    }
    __android_log_print(ANDROID_LOG_ERROR, "FlcyLog", "所有日志路径均不可写");
    return nullptr;
}

// 带时间戳写一行
// 常开 fd + 内存缓冲：避免每次 open/close 系统调用。
// 之前的实现每条日志都 open/write/close，
// 在 OnGUI 主线程里高频调用会明显拖慢甚至卡住。
// 首次打开时用 O_TRUNC：每次注入自动清空上一轮日志，
// 之后靠常开 fd + O_APPEND 追加，避免重复 open/close。
inline int LogFd() {
    static int s_fd = -2;    // -2=未尝试 -1=失败 >=0=已开
    if (s_fd != -2) return s_fd;
    const char* path = ResolvePath();
    if (!path) { s_fd = -1; return -1; }
    s_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    return s_fd;
}

inline void Write(const char* tag, const char* fmt, ...) {
    // 两个开关都关 → 直接返回，连格式化都省
    if (!kFileLogOn && !kLogcatOn) return;

    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (kLogcatOn) __android_log_print(ANDROID_LOG_INFO, tag, "%s", msg);

    if (!kFileLogOn) return;              // 落盘总开关关闭
    if (!TagAllowed(tag)) return;         // 不在白名单里的 tag 不落盘

    const int fd = LogFd();
    if (fd < 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;

    char line[520];
    const int n = snprintf(line, sizeof(line), "[%ld.%03ld][%s] %s\n",
                           ms / 1000, ms % 1000, tag, msg);
    if (n <= 0) return;

    static char buf[8192];
    static size_t blen = 0;
    const size_t need = (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);

    // 缓冲满了或装不下就 flush
    if (blen + need > sizeof(buf)) {
        if (blen) { write(fd, buf, blen); blen = 0; }
        // 单条超长直接写
        if (need > sizeof(buf)) { write(fd, line, need); return; }
    }
    memcpy(buf + blen, line, need);
    blen += need;

    // 每条立即落盘：用常开 fd，open/close 的开销已经省掉了，
    // 保留缓冲会导致崩溃时最后几条日志丢失，不利于排查。
    if (blen) {
        write(fd, buf, blen);
        blen = 0;
    }
}

// 强制落盘（崩溃前用）
inline void Flush() {
    if (!kFileLogOn) return;      // 落盘关了就不碰文件
    (void)LogFd();
}

// 清空日志（用户想重新排查时用）
inline void Reset() {
    if (!kFileLogOn) return;      // 落盘关了就不碰文件
    const char* path = ResolvePath();
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) close(fd);
    // 常开 fd 还指着同一个 inode，清空后位置要归零
    const int lfd = LogFd();
    if (lfd >= 0) lseek(lfd, 0, SEEK_END);
}

}  // namespace flcylog

#define FLOG(...)      flcylog::Write("FlcyUI", __VA_ARGS__)
#define FLOG_FEAT(...) flcylog::Write("FlcyFeat", __VA_ARGS__)
