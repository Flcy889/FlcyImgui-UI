#pragma once
#include <jni.h>
#include <dlfcn.h>
#include <cstdint>

// Mirrors imgui-liquid-glass src/ui/renderer/DisplayInfo.cpp (toucher-style).
// Query the Android Display natural size + rotation via JNI and expose the
// inverse input transform. Raw input coordinates are in natural orientation;
// the transform rotates them into the render-surface orientation.

namespace touch {

struct DisplayInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t rotation = -1;
    bool detected = false;
};

typedef jint (*JNI_GetCreatedJavaVMsFn)(JavaVM**, jsize, jsize*);

inline JavaVM* javaVm() {
    static JavaVM* vm = nullptr;
    if (vm) return vm;
    void* h = dlopen("libnativehelper.so", RTLD_NOW);
    if (!h) h = dlopen("libart.so", RTLD_NOW);
    if (!h) return nullptr;
    auto fn = (JNI_GetCreatedJavaVMsFn)dlsym(h, "JNI_GetCreatedJavaVMs");
    if (!fn) return nullptr;
    JavaVM* candidate = nullptr;
    jsize count = 0;
    if (fn(&candidate, 1, &count) != JNI_OK || count < 1 || !candidate)
        return nullptr;
    vm = candidate;
    return vm;
}

inline DisplayInfo queryDisplayInfo() {
    static DisplayInfo cached;
    static bool queried = false;
    // ★ 只有成功才锁定缓存。失败的话下次还能重试 ——
    //   之前失败也置 queried=true，导致永久返回空值（rot=-1 disp=0x0）
    if (queried && cached.detected) return cached;

    DisplayInfo info;
    JavaVM* vm = javaVm();
    if (!vm) { queried = true; return cached; }

    JNIEnv* env = nullptr;
    bool attached = false;
    jint status = vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (status != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { queried = true; return cached; }
        attached = true;
    }
    int32_t rotation = -1;
    int32_t width = 0, height = 0;
    do {
        jclass at = env->FindClass("android/app/ActivityThread");
        if (!at || env->ExceptionCheck()) break;
        jmethodID curApp = env->GetStaticMethodID(at, "currentApplication",
                                                  "()Landroid/app/Application;");
        if (!curApp || env->ExceptionCheck()) break;
        jobject app = env->CallStaticObjectMethod(at, curApp);
        if (!app || env->ExceptionCheck()) break;

        jclass ctx = env->FindClass("android/content/Context");
        jmethodID gss = env->GetMethodID(ctx, "getSystemService",
                                         "(Ljava/lang/String;)Ljava/lang/Object;");
        if (!gss || env->ExceptionCheck()) break;
        jstring svc = env->NewStringUTF("window");
        jobject wm = env->CallObjectMethod(app, gss, svc);
        if (!wm || env->ExceptionCheck()) break;

        jclass wmClass = env->GetObjectClass(wm);
        jmethodID gdd = env->GetMethodID(wmClass, "getDefaultDisplay",
                                         "()Landroid/view/Display;");
        if (!gdd || env->ExceptionCheck()) break;
        jobject display = env->CallObjectMethod(wm, gdd);
        if (!display || env->ExceptionCheck()) break;

        jclass dispClass = env->GetObjectClass(display);
        jmethodID getRot = env->GetMethodID(dispClass, "getRotation", "()I");
        jmethodID getRealSize = env->GetMethodID(dispClass, "getRealSize",
                                                 "(Landroid/graphics/Point;)V");
        if (!getRot || !getRealSize || env->ExceptionCheck()) break;

        jclass ptClass = env->FindClass("android/graphics/Point");
        jmethodID ptInit = env->GetMethodID(ptClass, "<init>", "()V");
        jfieldID ptX = env->GetFieldID(ptClass, "x", "I");
        jfieldID ptY = env->GetFieldID(ptClass, "y", "I");
        if (!ptInit || !ptX || !ptY || env->ExceptionCheck()) break;

        rotation = env->CallIntMethod(display, getRot);
        jobject pt = env->NewObject(ptClass, ptInit);
        env->CallVoidMethod(display, getRealSize, pt);
        width = env->GetIntField(pt, ptX);
        height = env->GetIntField(pt, ptY);
    } while (false);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();

    if (rotation >= 0 && rotation <= 3 && width > 0 && height > 0) {
        info.width = (uint32_t)width;
        info.height = (uint32_t)height;
        info.rotation = rotation;
        info.detected = true;
        cached = info;
        queried = true;         // 成功才锁定
        return cached;
    }

    // ★ 失败：返回空值但不锁定，下次重试
    return info;
}

}  // namespace touch
