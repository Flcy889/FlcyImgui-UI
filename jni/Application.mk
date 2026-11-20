APP_ABI := arm64-v8a
APP_PLATFORM := android-24
APP_STL := c++_static
APP_CPPFLAGS := -frtti -fexceptions
APP_OPTIM := release

# libc++ 在本 NDK 里需要显式给 locale rune table，否则 <fstream>/<vector>
# 会因 __locale 展开失败而报 "unknown rune table for this platform"。
APP_CPPFLAGS += -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE
