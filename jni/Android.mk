LOCAL_PATH := $(call my-dir)

# ============================================================================
# Android.mk —— ImGui 覆盖层 UI 框架
# ============================================================================
# 只编译三类东西：
#   ① ImGui 核心 + Android/OpenGL3 后端（公开代码，原样）
#   ② 第三方基础库：Dobby（inline hook，用于接管 eglSwapBuffers）、xdl（符号解析）
#   ③ 本项目自己的 UI 代码：ui/ui_overlay.cpp（渲染接管）、ui/ui_menu.cpp（界面）
#
#   ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./jni/Android.mk \
#             NDK_APPLICATION_MK=./jni/Application.mk APP_ABI=arm64-v8a
#
# 产物：libs/arm64-v8a/libimgui-overlay.so
# ============================================================================

# ============================================================================
# Dobby（预编译静态库，作为覆盖层的 inline hook 后端）
# ============================================================================
include $(CLEAR_VARS)
LOCAL_MODULE := libdobby
LOCAL_SRC_FILES := Dobby/$(TARGET_ARCH_ABI)/libdobby.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/Dobby
include $(PREBUILT_STATIC_LIBRARY)

# ============================================================================
# ImGui 核心 + xdl —— 永不混淆、不改动
# ============================================================================
# 单独成模块的理由：
#   1. ImGui 三万多行，改动它没有意义，升级时直接整体替换
#   2. xdl 是 linker 实现，不能动
# ============================================================================
include $(CLEAR_VARS)
LOCAL_MODULE := imgui_core

LOCAL_SRC_FILES := \
    ImGui/imgui.cpp \
    ImGui/imgui_draw.cpp \
    ImGui/imgui_tables.cpp \
    ImGui/imgui_widgets.cpp \
    ImGui/imgui_impl_opengl3.cpp \
    ImGui/imgui_impl_android.cpp \
    xdl/xdl.c \
    xdl/xdl_iterate.c \
    xdl/xdl_linker.c \
    xdl/xdl_lzma.c \
    xdl/xdl_util.c

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/ImGui \
    $(LOCAL_PATH)/xdl \
    $(LOCAL_PATH)/xdl/include \
    $(LOCAL_PATH)/Dobby

LOCAL_CFLAGS   := -DIMGUI_IMPL_OPENGL_ES3 -fvisibility=hidden -w
LOCAL_CPPFLAGS := -std=c++17 -fno-rtti -fexceptions \
                  -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -w
LOCAL_EXPORT_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/ImGui

include $(BUILD_STATIC_LIBRARY)

# ============================================================================
# 主模块 —— UI 框架本体
# ============================================================================
include $(CLEAR_VARS)
LOCAL_MODULE := imgui-overlay

LOCAL_SRC_FILES := \
    ui/ui_overlay.cpp \
    ui/ui_menu.cpp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/ImGui \
    $(LOCAL_PATH)/xdl \
    $(LOCAL_PATH)/xdl/include \
    $(LOCAL_PATH)/Dobby

LOCAL_CFLAGS   := -DIMGUI_IMPL_OPENGL_ES3 -fvisibility=hidden -w
LOCAL_CPPFLAGS := -std=c++17 -fno-rtti -fexceptions \
                  -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -w

# ★ 这两个链接参数不能去掉：
#   max-page-size=16384  Android 15+ 要求 16KB 页对齐
#   --no-gc-sections     Dobby 只用函数指针引用绘制入口，GC 会把整条绘制链删掉
#                        （症状：so 编出来了，但屏幕上一片空白）
LOCAL_LDFLAGS := \
    -Wl,-z,max-page-size=16384 \
    -Wl,--no-gc-sections \
    -Wl,-z,noexecstack \
    -Wl,-z,relro,-z,now

LOCAL_LDLIBS := -lGLESv3 -lEGL -llog -landroid -ldl -lz
LOCAL_STATIC_LIBRARIES := libdobby imgui_core

include $(BUILD_SHARED_LIBRARY)

$(info ============================================================)
$(info  ImGui 覆盖层 UI 框架)
$(info   ABI    : $(TARGET_ARCH_ABI))
$(info   源文件 : ui/ui_overlay.cpp ui/ui_menu.cpp)
$(info ============================================================)
