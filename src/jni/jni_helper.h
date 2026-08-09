#pragma once
#include <jni.h>
#include <Windows.h>
#include <cstdio>

typedef jint(JNICALL* fnGetCreatedJavaVMs)(JavaVM**, jsize, jsize*);

// =================================================================
// JNIHelper
// =================================================================
// Attaches our client thread to the JVM that Lunar is already
// running in.
//
// DAEMON MATTERS HERE
// AttachCurrentThread creates a non-daemon thread, and the JVM
// refuses to exit while one is alive. If the player closed
// Minecraft without ejecting first, javaw would sit there forever
// as a zombie process. AttachCurrentThreadAsDaemon lets the JVM
// shut down whenever it likes and simply abandons our thread.
// =================================================================

class JNIHelper {
private:
    inline static JavaVM* s_jvm = nullptr;
    inline static JNIEnv* s_env = nullptr;
    inline static bool s_attached = false;

public:
    static bool Initialize() {
        HMODULE hJVM = GetModuleHandleA("jvm.dll");
        if (!hJVM) {
            printf("[JNI] jvm.dll not loaded in this process\n");
            return false;
        }

        auto getVMs = (fnGetCreatedJavaVMs)GetProcAddress(hJVM, "JNI_GetCreatedJavaVMs");
        if (!getVMs) {
            printf("[JNI] JNI_GetCreatedJavaVMs not exported\n");
            return false;
        }

        jsize vmCount = 0;
        if (getVMs(&s_jvm, 1, &vmCount) != JNI_OK || vmCount == 0 || !s_jvm) {
            printf("[JNI] no running JVM found\n");
            return false;
        }

        // Already attached? Reuse the env rather than attaching twice.
        if (s_jvm->GetEnv((void**)&s_env, JNI_VERSION_1_6) == JNI_OK && s_env) {
            s_attached = false;   // we did not create it, so we must not detach it
            return true;
        }

        jint result = s_jvm->AttachCurrentThreadAsDaemon((void**)&s_env, nullptr);
        if (result != JNI_OK) {
            printf("[JNI] AttachCurrentThreadAsDaemon failed: %d\n", result);
            return false;
        }

        s_attached = true;
        return true;
    }

    static JNIEnv* GetEnv()     { return s_env; }
    static JavaVM* GetVM()      { return s_jvm; }
    static int     GetVersion() { return s_env ? s_env->GetVersion() : 0; }

    // For any extra thread that needs JNI. Must be paired with
    // DetachThread before that thread exits.
    static JNIEnv* AttachThread() {
        if (!s_jvm) return nullptr;
        JNIEnv* env = nullptr;
        if (s_jvm->AttachCurrentThreadAsDaemon((void**)&env, nullptr) != JNI_OK)
            return nullptr;
        return env;
    }

    static void DetachThread() {
        if (s_jvm) s_jvm->DetachCurrentThread();
    }

    static void Detach() {
        if (s_attached && s_jvm) {
            s_jvm->DetachCurrentThread();
            s_attached = false;
        }
        s_env = nullptr;
    }
};
