#pragma once
#include <jni.h>
#include <Windows.h>
#include <cstdio>

typedef jint(JNICALL* fnGetCreatedJavaVMs)(JavaVM**, jsize, jsize*);

class JNIHelper {
private:
    inline static JavaVM* s_jvm = nullptr;
    inline static JNIEnv* s_env = nullptr;
    inline static bool s_attached = false;

public:
    static bool Initialize() {
        HMODULE hJVM = GetModuleHandleA("jvm.dll");
        if (!hJVM) {
            printf("[JNI] jvm.dll not found\n");
            return false;
        }

        auto getVMs = (fnGetCreatedJavaVMs)GetProcAddress(hJVM, "JNI_GetCreatedJavaVMs");
        if (!getVMs) return false;

        jsize vmCount = 0;
        if (getVMs(&s_jvm, 1, &vmCount) != JNI_OK || vmCount == 0) {
            printf("[JNI] No JVMs found\n");
            return false;
        }

        jint result = s_jvm->AttachCurrentThread((void**)&s_env, nullptr);
        if (result != JNI_OK) {
            printf("[JNI] AttachCurrentThread failed: %d\n", result);
            return false;
        }

        s_attached = true;
        return true;
    }

    static JNIEnv* GetEnv()     { return s_env; }
    static JavaVM* GetVM()      { return s_jvm; }
    static int     GetVersion() { return s_env ? s_env->GetVersion() : 0; }

    static JNIEnv* AttachThread() {
        JNIEnv* env = nullptr;
        s_jvm->AttachCurrentThread((void**)&env, nullptr);
        return env;
    }

    static void DetachThread() {
        s_jvm->DetachCurrentThread();
    }

    static void Detach() {
        if (s_attached && s_jvm) {
            s_jvm->DetachCurrentThread();
            s_attached = false;
        }
    }
};
