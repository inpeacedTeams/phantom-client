#pragma once
#include <jni.h>
#include <jvmti.h>
#include <atomic>
#include <cstring>
#include <cstdio>

#include "minecraft.h"
#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"

// =================================================================
// Cursor
// =================================================================
// WHY THE MENU HAD NO USABLE CURSOR
//
// While you are in game, Minecraft calls Mouse.setGrabbed(true).
// A grabbed mouse has no position: LWJGL hides the pointer and the
// game re-centres it every frame so it can read pure deltas.
//
// ImGui's Win32 backend gets its cursor position from GetCursorPos.
// Against a grabbed mouse that returns the centre of the window,
// over and over, so the menu cursor sat pinned in the middle and
// nothing could be clicked. Drawing a software cursor did not help,
// because the problem was never the drawing.
//
// The fix is the same thing the game does when it opens a chest:
// ungrab the mouse and clear inGameHasFocus. Ungrabbing restores a
// real pointer, and clearing the focus flag stops runTick from
// feeding mouse movement into the camera, which would otherwise
// spin the player around while you drag a slider.
//
// THREADING
// This is JNI, so it may only run on the client thread. The render
// thread just sets an atomic and the next tick reconciles it.
// =================================================================

class Cursor {
private:
    inline static jclass    s_mouseClass = nullptr;   // global ref
    inline static jmethodID s_setGrabbed = nullptr;
    inline static jmethodID s_isGrabbed  = nullptr;
    inline static jfieldID  s_fInGameFocus = nullptr;

    inline static bool s_resolved = false;
    inline static bool s_usable   = false;

    // What the UI wants, set from the render thread
    inline static std::atomic<bool> s_wantRelease{ false };

    // What we have actually done, owned by the client thread
    inline static bool s_released = false;

    // Lunar's classloader hides LWJGL from env->FindClass, so the
    // class has to be found the same way Minecraft's own are.
    static jclass FindBySignature(JNIEnv* env, const char* wantSig) {
        jvmtiEnv* jvmti = JvmtiUtil::Get(env);
        if (!jvmti) return nullptr;

        jint count = 0;
        jclass* classes = nullptr;
        if (jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE || !classes)
            return nullptr;

        jclass found = nullptr;
        for (jint i = 0; i < count && !found; i++) {
            char* sig = nullptr;
            if (jvmti->GetClassSignature(classes[i], &sig, nullptr) == JVMTI_ERROR_NONE && sig) {
                if (std::strcmp(sig, wantSig) == 0)
                    found = (jclass)env->NewGlobalRef(classes[i]);
                jvmti->Deallocate((unsigned char*)sig);
            }
        }
        jvmti->Deallocate((unsigned char*)classes);
        return found;
    }

    static void Resolve(JNIEnv* env) {
        if (s_resolved) return;
        s_resolved = true;

        s_mouseClass = FindBySignature(env, "Lorg/lwjgl/input/Mouse;");
        if (s_mouseClass) {
            s_setGrabbed = env->GetStaticMethodID(s_mouseClass, "setGrabbed", "(Z)V");
            if (env->ExceptionCheck()) { env->ExceptionClear(); s_setGrabbed = nullptr; }

            s_isGrabbed = env->GetStaticMethodID(s_mouseClass, "isGrabbed", "()Z");
            if (env->ExceptionCheck()) { env->ExceptionClear(); s_isGrabbed = nullptr; }
        }

        if (ClassResolver::mcClass) {
            s_fInGameFocus = JvmtiUtil::FindField(env, ClassResolver::mcClass,
                { "field_71415_G", "inGameHasFocus" });
        }

        s_usable = (s_setGrabbed != nullptr);

        printf("[Cursor] setGrabbed=%p focusField=%p usable=%d\n",
            (void*)s_setGrabbed, (void*)s_fInGameFocus, (int)s_usable);

        if (!s_usable)
            printf("[Cursor] WARN: cannot ungrab the mouse, the menu will be hard to click\n");
    }

    static void SetGrabbed(JNIEnv* env, bool grabbed) {
        if (!s_setGrabbed) return;
        env->CallStaticVoidMethod(s_mouseClass, s_setGrabbed, (jboolean)grabbed);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    static void SetGameFocus(JNIEnv* env, bool focus) {
        if (!s_fInGameFocus) return;
        jobject mc = Minecraft::GetInstance(env);
        if (!mc) return;
        env->SetBooleanField(mc, s_fInGameFocus, (jboolean)focus);
    }

public:
    static bool IsUsable()  { return s_usable; }
    static bool IsReleased(){ return s_released; }

    // Called from the render thread. Cheap and lock free.
    static void RequestRelease(bool release) {
        s_wantRelease.store(release, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------
    // Reconcile on the client thread, once per tick.
    //
    // Only acts on a CHANGE. Calling setGrabbed every tick would
    // fight the game for control of the pointer and make the camera
    // jitter.
    // -------------------------------------------------------------
    static void Apply(JNIEnv* env) {
        Resolve(env);
        if (!s_usable) return;

        bool want = s_wantRelease.load(std::memory_order_relaxed);
        if (want == s_released) return;

        if (want) {
            SetGameFocus(env, false);   // stop feeding the camera
            SetGrabbed(env, false);     // give the pointer back
        } else {
            SetGrabbed(env, true);
            SetGameFocus(env, true);
        }
        s_released = want;
    }

    // Hand the mouse back no matter what we think the state is.
    // Used on eject and when leaving a world: a client that exits
    // while holding the pointer leaves the player unable to look
    // around.
    static void ForceRestore(JNIEnv* env) {
        if (!env || !s_usable) return;
        if (!s_released) return;

        SetGrabbed(env, true);
        SetGameFocus(env, true);
        s_released = false;
        s_wantRelease.store(false, std::memory_order_relaxed);
    }

    static void Shutdown(JNIEnv* env) {
        ForceRestore(env);
        if (env && s_mouseClass) env->DeleteGlobalRef(s_mouseClass);
        s_mouseClass = nullptr;
        s_setGrabbed = nullptr;
        s_isGrabbed  = nullptr;
        s_fInGameFocus = nullptr;
        s_resolved = false;
        s_usable = false;
    }
};
