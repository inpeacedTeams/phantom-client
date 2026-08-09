#pragma once
#include <jni.h>
#include <cstdio>

#include "minecraft.h"
#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"

// =================================================================
// KeyBinds
// =================================================================
// WHY THIS EXISTS
//
// Calling entity.setSprinting(false) does nothing lasting. Every
// tick EntityPlayerSP.onLivingUpdate() recomputes the sprint state
// from the movement keys and the sprint keybind, so our write is
// overwritten before the packet goes out. The same applies to
// sneaking and item use.
//
// Driving GameSettings.keyBind*.pressed puts us upstream of that
// logic. The game then produces exactly the packet sequence a real
// key press produces, which is both correct AND the reason none of
// this is visible to a server-side anticheat.
//
// A held key is a two-field state in 1.8:
//   pressed    is the key down right now
//   pressTime  queued press events, consumed by isPressed()
// We only touch pressed, which is what movement and item use read.
// =================================================================

class KeyBinds {
private:
    struct Bind {
        jobject  obj  = nullptr;   // global ref to the KeyBinding
        jfieldID pressed = nullptr;
        bool     overridden = false;
        bool     wanted = false;
    };

    inline static Bind s_forward, s_back, s_left, s_right;
    inline static Bind s_jump, s_sneak, s_sprint;
    inline static Bind s_useItem, s_attack;

    inline static jfieldID s_fPressed = nullptr;
    inline static bool s_ready = false;

    static void Load(JNIEnv* env, jobject gs, Bind& out,
                     const char* srg, const char* mcp)
    {
        if (!ClassResolver::gameSettings) return;

        jfieldID f = JvmtiUtil::FindField(env, ClassResolver::gameSettings, { srg, mcp });
        if (!f) return;

        jobject kb = env->GetObjectField(gs, f);
        if (!kb) return;

        out.obj = env->NewGlobalRef(kb);

        if (!s_fPressed) {
            jclass cls = env->GetObjectClass(kb);
            s_fPressed = JvmtiUtil::FindField(env, cls, { "field_74513_e", "pressed" });
            env->DeleteLocalRef(cls);
        }
        out.pressed = s_fPressed;

        env->DeleteLocalRef(kb);
    }

public:
    static bool IsReady() { return s_ready; }

    static bool Init(JNIEnv* env) {
        if (s_ready) return true;

        jobject gs = Minecraft::GetGameSettings(env);
        if (!gs) return false;

        Load(env, gs, s_forward,  "field_74351_w", "keyBindForward");
        Load(env, gs, s_back,     "field_74368_y", "keyBindBack");
        Load(env, gs, s_left,     "field_74370_x", "keyBindLeft");
        Load(env, gs, s_right,    "field_74366_z", "keyBindRight");
        Load(env, gs, s_jump,     "field_74314_A", "keyBindJump");
        Load(env, gs, s_sneak,    "field_74311_E", "keyBindSneak");
        Load(env, gs, s_sprint,   "field_151444_V", "keyBindSprint");
        Load(env, gs, s_useItem,  "field_74313_G", "keyBindUseItem");
        Load(env, gs, s_attack,   "field_74312_F", "keyBindAttack");

        s_ready = (s_forward.obj != nullptr && s_fPressed != nullptr);

        printf("[KeyBinds] ready=%d fwd=%p back=%p sneak=%p jump=%p use=%p sprint=%p\n",
            (int)s_ready, (void*)s_forward.obj, (void*)s_back.obj,
            (void*)s_sneak.obj, (void*)s_jump.obj,
            (void*)s_useItem.obj, (void*)s_sprint.obj);

        return s_ready;
    }

    static void Shutdown(JNIEnv* env) {
        Bind* all[] = { &s_forward, &s_back, &s_left, &s_right,
                        &s_jump, &s_sneak, &s_sprint, &s_useItem, &s_attack };
        for (Bind* b : all) {
            if (b->obj) { env->DeleteGlobalRef(b->obj); b->obj = nullptr; }
            b->overridden = false;
        }
        s_ready = false;
    }

    // ---- Raw state ----
    static bool Get(JNIEnv* env, Bind& b) {
        if (!b.obj || !b.pressed) return false;
        return env->GetBooleanField(b.obj, b.pressed) != 0;
    }

    static void Set(JNIEnv* env, Bind& b, bool down) {
        if (!b.obj || !b.pressed) return;
        env->SetBooleanField(b.obj, b.pressed, (jboolean)down);
        b.overridden = true;
        b.wanted = down;
    }

    // Hand a key back to the player's real input
    static void Release(JNIEnv* env, Bind& b) {
        if (!b.obj || !b.pressed || !b.overridden) return;
        env->SetBooleanField(b.obj, b.pressed, (jboolean)false);
        b.overridden = false;
    }

    // ---- Named accessors ----
    static void SetForward(JNIEnv* e, bool v) { Set(e, s_forward, v); }
    static void SetBack(JNIEnv* e, bool v)    { Set(e, s_back, v); }
    static void SetLeft(JNIEnv* e, bool v)    { Set(e, s_left, v); }
    static void SetRight(JNIEnv* e, bool v)   { Set(e, s_right, v); }
    static void SetJump(JNIEnv* e, bool v)    { Set(e, s_jump, v); }
    static void SetSneak(JNIEnv* e, bool v)   { Set(e, s_sneak, v); }
    static void SetSprint(JNIEnv* e, bool v)  { Set(e, s_sprint, v); }
    static void SetUseItem(JNIEnv* e, bool v) { Set(e, s_useItem, v); }
    static void SetAttack(JNIEnv* e, bool v)  { Set(e, s_attack, v); }

    static bool GetForward(JNIEnv* e) { return Get(e, s_forward); }
    static bool GetBack(JNIEnv* e)    { return Get(e, s_back); }
    static bool GetLeft(JNIEnv* e)    { return Get(e, s_left); }
    static bool GetRight(JNIEnv* e)   { return Get(e, s_right); }
    static bool GetJump(JNIEnv* e)    { return Get(e, s_jump); }
    static bool GetSneak(JNIEnv* e)   { return Get(e, s_sneak); }
    static bool GetSprint(JNIEnv* e)  { return Get(e, s_sprint); }
    static bool GetUseItem(JNIEnv* e) { return Get(e, s_useItem); }
    static bool GetAttack(JNIEnv* e)  { return Get(e, s_attack); }

    static void ReleaseForward(JNIEnv* e) { Release(e, s_forward); }
    static void ReleaseBack(JNIEnv* e)    { Release(e, s_back); }
    static void ReleaseLeft(JNIEnv* e)    { Release(e, s_left); }
    static void ReleaseRight(JNIEnv* e)   { Release(e, s_right); }
    static void ReleaseJump(JNIEnv* e)    { Release(e, s_jump); }
    static void ReleaseSneak(JNIEnv* e)   { Release(e, s_sneak); }
    static void ReleaseSprint(JNIEnv* e)  { Release(e, s_sprint); }
    static void ReleaseUseItem(JNIEnv* e) { Release(e, s_useItem); }

    // True when the module can actually drive movement keys
    static bool HasMovement() { return s_forward.obj && s_back.obj; }
    static bool HasSneak()    { return s_sneak.obj != nullptr; }
    static bool HasJump()     { return s_jump.obj != nullptr; }
    static bool HasUseItem()  { return s_useItem.obj != nullptr; }
};
