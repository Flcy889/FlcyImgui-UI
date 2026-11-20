#!/usr/bin/env bash
# ============================================================================
# build_android.sh —— 不依赖 ndk-build 的直接交叉编译（arm64-v8a）
# ============================================================================
# 用途：
#   · CI / 没有 NDK 构建系统时，用 NDK 自带的 sysroot 直接喂 clang
#   · 产物与 Android.mk 一致：libs/arm64-v8a/libimgui-overlay.so
#
# 用法：
#   NDK_SYSROOT=/path/to/ndk/toolchains/llvm/prebuilt/<host>/sysroot \
#   ./tools/build_android.sh
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JNI="$ROOT/jni"
OUT="$ROOT/libs/arm64-v8a"
OBJ="$ROOT/obj/arm64-v8a"

: "${NDK_SYSROOT:=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/sysroot}"
: "${CC:=clang}"
: "${CXX:=clang++}"

TRIPLE=aarch64-linux-android24
COMMON=(--target="$TRIPLE" --sysroot="$NDK_SYSROOT" -O2 -fPIC -w
        -I"$JNI" -I"$JNI/ImGui" -I"$JNI/xdl" -I"$JNI/xdl/include" -I"$JNI/Dobby")
CFLAGS=("${COMMON[@]}" -DIMGUI_IMPL_OPENGL_ES3)
CXXFLAGS=("${COMMON[@]}" -std=c++17 -fno-rtti -fexceptions
          -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -DIMGUI_IMPL_OPENGL_ES3)

mkdir -p "$OUT" "$OBJ"

C_SRCS=(xdl/xdl.c xdl/xdl_iterate.c xdl/xdl_linker.c xdl/xdl_lzma.c xdl/xdl_util.c)
CXX_SRCS=(ImGui/imgui.cpp ImGui/imgui_draw.cpp ImGui/imgui_tables.cpp
          ImGui/imgui_widgets.cpp ImGui/imgui_impl_opengl3.cpp
          ImGui/imgui_impl_android.cpp
          ui/ui_menu.cpp ui/ui_overlay.cpp)

OBJS=()
for s in "${C_SRCS[@]}"; do
    o="$OBJ/$(echo "${s%.c}" | tr '/' '_').o"
    echo "[cc]  $s"
    "$CC"  "${CFLAGS[@]}"   -c "$JNI/$s" -o "$o"
    OBJS+=("$o")
done
for s in "${CXX_SRCS[@]}"; do
    o="$OBJ/$(echo "${s%.cpp}" | tr '/' '_').o"
    echo "[cxx] $s"
    "$CXX" "${CXXFLAGS[@]}" -c "$JNI/$s" -o "$o"
    OBJS+=("$o")
done

echo "[ld]  libimgui-overlay.so"
# 说明：部分精简过的 sysroot 里没有 -lgcc / libunwind，
# 所以这里用 -nodefaultlibs 显式列库；完整 NDK 环境下同样可用。
"$CXX" --target="$TRIPLE" --sysroot="$NDK_SYSROOT" -shared -nodefaultlibs \
    -o "$OUT/libimgui-overlay.so" "${OBJS[@]}" "$JNI/Dobby/arm64-v8a/libdobby.a" \
    -Wl,-z,max-page-size=16384 -Wl,--no-gc-sections \
    -Wl,-z,noexecstack -Wl,-z,relro,-z,now \
    -lGLESv3 -lEGL -llog -landroid -ldl -lz -lc++ -lc++abi -lc -lm

echo "OK -> $OUT/libimgui-overlay.so"
ls -l "$OUT/libimgui-overlay.so"
