# ImGui Overlay UI（KiHan imguimod 清理版）

一套可以直接挂到任意 Android + OpenGL ES 应用上的 **ImGui 覆盖层界面框架**。
来自 `KiHan/khimgui/imguimod` 这个工程，**保留原版 UI 体系，外挂功能整体剔除**。

- 原工程：带游戏内存读写、il2cpp 方法 hook、防封、卡密联网验证的功能菜单（so 约 8.8 MB）
- 本工程：同样一套界面（主题 / 控件 / 分页 / 悬浮球 / 模糊 / 手势），
  **没有一行代码认识具体游戏**，只剩渲染接管 + 界面本身

```
jni/
├── ui/
│   ├── ui_layer.h        两层之间的唯一接口（26 行）
│   ├── ui_overlay.cpp    渲染接管层：EGL hook + ImGui 帧循环（777 行）
│   └── ui_menu.cpp       界面本体：主题 / 控件 / 分页 / 悬浮球 / 交互（1449 行）
├── ImGui/                ImGui 1.92.0 WIP + Android / OpenGL3 后端（原样）
├── Dobby/                inline hook 库（接管 eglSwapBuffers 用）
├── xdl/                  符号解析（原样）
├── ui_prefs.h            界面偏好（落盘 / 主题色 / 圆角 / 缩放 / 悬浮球位置）
├── touch_calib.h         触摸坐标校准
├── touch_display.h       屏幕 / 旋转信息
├── touch_transform.h     旋转矩阵
├── gl_probe.h            GL 状态探针
├── gl_blur.h             面板模糊（抓区域做高斯 + 圆角保形）
├── flcy_log.h            落盘日志（logcat + 文件）
├── repair_font.h         中文字体数据（21 MB，编译进 so）
└── ball_texture.h        悬浮球贴图（1.3 MB）
```

## 保留了什么

| 模块 | 说明 |
|---|---|
| 主题系统 | 深色 / 纯黑 / 浅色三态；6 个主题色预设；主题色「纯黑」时 accent 自动反相 |
| 自绘控件 | iOS 风开关（220ms 贝塞尔过冲 + 弹簧按压）、滑条、按钮、分段标题、说明段、键值行 |
| 分页 | 5 个分区，切换时双页同时插值竖向滚过；侧滑条 / 滚轮 / 拖拽三种滚动 |
| 开合动画 | 悬浮球 → 菜单的缩放+位移+透明度三段联动，开用 decelerate、关用 accelerate |
| 悬浮球 | 拖拽移动（落盘位置）、点击开菜单；菜单内点左上图片关闭、长按拖动、右下角自由缩放 |
| 面板模糊 | 只抓菜单矩形做高斯，用 `AddImageRounded` 画 → 圆角不变方角 |
| 手势 / 坐标 | MotionEvent 转发 + 旋转矩阵校准，横竖屏都能点准 |
| 稳定性 | 段级 SIGSEGV 保护（崩了跳过这一段）、分级降级、swap hook 60 秒重试 |

## 剔除了什么

| 文件 / 模块 | 原用途 | 处理 |
|---|---|---|
| `features.cpp`（117 KB） | 功能调度、方法解析、Job 队列 | 删除 |
| `hook.cpp` 的 il2cpp 部分 | `KHLuaGlobalController.OnGUI` hook、按基址+偏移调游戏方法 | 删除 |
| `practice_all.cpp`（55 KB） | 全忍者（10 hook + 12 方法） | 删除 |
| `tersafe.cpp` / `TersafeHooks.h` | 反作弊 / 防封 | 删除 |
| `PerfectSwap.cpp` / `perfect_copy.h` / `idswap.h` | 完美改、ID 替换 | 删除 |
| `keyout_lua.h` / `kfb_replace.h` / `pure/` | Lua 密钥注入、KFB 数据表替换 | 删除 |
| `auth.cpp` / `deviceid.cpp` / `01_卡密服务端/` | 卡密联网验证 + 管理后台 | 删除 |
| `IL2CPPHelper.*` / `IL2CPP_API/`（839 文件） | il2cpp 头文件与封装 | 删除 |
| `offsets_179.hpp` / `addr_resolver.hpp` / `sym_resolve.h` / `il2cpp_rt.*` | 游戏符号与偏移解析 | 删除 |
| `ytbl/` | 液态玻璃（v140 已停用） | 删除 |
| `04_OLLVM工具包/` `03_Android16补丁/` `02_工具脚本/` | so 混淆加固、版本补丁脚本 | 删除 |
| `06_so产物/`（26 MB） | 原外挂二进制 | 删除（不随仓库分发） |
| `账号信息.md` / 服务器地址 / 卡密后台 | 运维凭据 | 删除 |

目录里有 `docs/清理清单.md`，逐条对应上表，附原始路径。

界面侧只有一处**行为替换**：原来「实用 / 娱乐 / 素材 / 调试」四个分区全是游戏开关，
现在换成「概览 / 控件 / 布局 / 主题」四个纯 UI 演示分区（开关、滑条、按钮、色板、落盘）。
「外观」分区原样保留。

## 编译

### A. ndk-build（推荐）

```bash
ndk-build NDK_PROJECT_PATH=. \
          APP_BUILD_SCRIPT=./jni/Android.mk \
          NDK_APPLICATION_MK=./jni/Application.mk \
          APP_ABI=arm64-v8a -j4
# 产物：libs/arm64-v8a/libimgui-overlay.so
```

要求 NDK r27 及以上（`APP_PLATFORM := android-24`，STL `c++_static`）。
路径请放在纯 ASCII 目录下（中文路径会让 ndk-build 报错）。

### B. 直接喂 clang（没有 ndk-build 时）

```bash
NDK_SYSROOT=/path/to/ndk/toolchains/llvm/prebuilt/<host>/sysroot \
  ./tools/build_android.sh
```

### 两个不能删的链接参数

```make
-Wl,-z,max-page-size=16384   # Android 15+ 的 16 KB 页要求
-Wl,--no-gc-sections         # Dobby 只用函数指针引用绘制入口，
                             # GC 会把整条绘制链当死代码删掉（症状：so 有，屏幕空白）
```

## 接入自己的应用

`ui/ui_overlay.cpp` 的 `Init()` 是 `__attribute__((constructor))`，
so 被加载进目标进程后自动挂 `eglSwapBuffers`（以及 `WithDamageKHR` / `2KHR`），
之后每帧调 `ui_layer::Frame(W, H, dt)` 完成绘制。

```cpp
// 宿主侧可选：菜单展开时吞掉自己的点击
if (ui_layer::MenuOpen()) { /* ... */ }
```

偏好落盘默认 `/sdcard/imgui_ui.txt`，可编译期改：

```make
LOCAL_CPPFLAGS += -DIMGUI_UI_PREFS_PATH='"/data/local/tmp/imgui_ui.txt"'
```

日志：`logcat` + `/sdcard/imgui_ui_log.txt`（只落 `ImGuiInit` / `ImGuiHook` / `ImGuiUI` 三个 tag）。

## 已知限制

- 只支持 **EGL + OpenGL ES** 的宿主；Vulkan 渲染的进程挂不上（日志会明确报出来）
- `repair_font.h` 是 21 MB 的中文字体数据，会整体编进 so —— 不需要中文可换掉
- 触摸来自 `MotionEvent::copyFrom` hook；个别机型符号被裁掉时点不动，日志里有 `输入 hook = -1`
- 悬浮球贴图 1.3 MB（`ball_texture.h`），不需要可以换成纯色圆

## 第三方

| 组件 | 许可 |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.0 WIP | MIT |
| [Dobby](https://github.com/jmpews/Dobby) | Apache-2.0 |
| [xdl](https://github.com/hexhacking/xdl) | Apache-2.0 |

本仓库自己写的代码（`jni/ui/`、`ui_prefs.h`、`touch_*.h`、`gl_*.h`、`flcy_log.h`）以 MIT 发布，见 `LICENSE`。

## 免责

从一份游戏功能菜单里剥离出来的界面层。用途是给自己应用做调试面板 / 覆盖层 UI。
仓库内不含任何游戏符号、偏移、内存读写、网络验证或外挂功能。
