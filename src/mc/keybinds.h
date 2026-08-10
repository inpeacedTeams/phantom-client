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
// sneaking, and to writing moveForward / moveStrafing by hand.
//
// Driving GameSettings.keyBind*.pressed puts us upstream of that
// logic. The game then produces exactly the packet sequence a real
// key press produces, which is both correct AND the reason none of
// this is visible to a server-side anticheat.
//
// A held key is a two-field state in 1.8:
//
//   pressed     is the key down right now
//   pressTime   how many press EVENTS are queued
//
// Those two do completely different jobs and the difference is the
// whole reason the autoclicker never worked.
//
//   Minecraft.runTick():
//       while (gameSettings.keyBindAttack.isPressed()) clickMouse();
//
//   KeyBinding.isPressed():
//       if (pressTime == 0) return false;
//       --pressTime; return true;
//
// So pressTime is a COUNTER OF CLICKS. Adding 1 to it makes the
// game call clickMouse() exactly once on its next tick, running the
// real swingItem plus attackEntity path. Holding `pressed` down
// does not attack at all; it only mines blocks.
//
// This is why the client no longer synthesises OS mouse input:
// pressTime is the same door a physical click comes through, it
// cannot be told apart from one, and it works no matter what the
// game is doing with raw input.
//
// IMPORTANT
// A key we force down stays down until we clear it. Every module
// that holds one must release it in OnDisable, and ReleaseAll()
// exists as the backstop for eject and disconnect.
// =================================================================

class KeyBinds {
private:
    struct Bind {
        jobject  obj = nullptr;      // global ref to the KeyBinding
        jfieldID pressed = nullptr;
        bool     overridden = false; // we are the ones holding it
        bool     wanted = false;
    };

    inline static Bind s_forward, s_back, s_left, s_right;
    inline static Bind s_jump, s_sneak, s_sprint;
    inline static Bind s_useItem, s_attack;

    inline static jfieldID s_fPressed   = nullptr;
    inline static jfieldID s_fPressTime = nullptr;
    inline static bool s_ready = false;

    static Bind** AllPtrs(int& count) {
        static Bind* table[] = {
            &s_forward, &s_back, &s_left, &s_right,
            &s_jump, &s_sneak, &s_sprint, &s_useItem, &s_attack
        };
        count = (int)(sizeof(table) / sizeof(table[0]));
        return table;
    }

    static void Load(JNIEnv* env, jobject gs, Bind& out,
                     const char* srg, const char* mcp)
    {
        if (out.obj) return;                    // already resolved
        if (!ClassResolver::gameSettings) return;

        jfieldID f = JvmtiUtil::FindField(env, ClassResolver::gameSettings, { srg, mcp });
        if (!f) return;

        jobject kb = env->GetObjectField(gs, f);
        if (!kb) return;

        out.obj = env->NewGlobalRef(kb);

        if (!s_fPressed || !s_fPressTime) {
            jclass cls = env->GetObjectClass(kb);
            if (!s_fPressed)
                s_fPressed = JvmtiUtil::FindField(env, cls, { "field_74513_e", "pressed" });
            if (!s_fPressTime)
                s_fPressTime = JvmtiUtil::FindField(env, cls,
                    { "field_151474_i", "pressTime", "field_74512_d" });
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

        Load(env, gs, s_forward, "field_74351_w",  "keyBindForward");
        Load(env, gs, s_back,    "field_74368_y",  "keyBindBack");
        Load(env, gs, s_left,    "field_74370_x",  "keyBindLeft");
        Load(env, gs, s_right,   "field_74366_z",  "keyBindRight");
        Load(env, gs, s_jump,    "field_74314_A",  "keyBindJump");
        Load(env, gs, s_sneak,   "field_74311_E",  "keyBindSneak");
        Load(env, gs, s_sprint,  "field_151444_V", "keyBindSprint");
        Load(env, gs, s_useItem, "field_74313_G",  "keyBindUseItem");
        Load(env, gs, s_attack,  "field_74312_F",  "keyBindAttack");

        s_ready = (s_forward.obj != nullptr && s_fPressed != nullptr);

        if (s_ready) {
            printf("[KeyBinds] resolved fwd=%p sneak=%p jump=%p use=%p attack=%p "
                   "pressed=%p pressTime=%p\n",
                (void*)s_forward.obj, (void*)s_sneak.obj, (void*)s_jump.obj,
                (void*)s_useItem.obj, (void*)s_attack.obj,
                (void*)s_fPressed, (void*)s_fPressTime);

            if (!s_fPressTime)
                printf("[KeyBinds] WARN: pressTime unresolved, native clicking is off\n");
        }
        return s_ready;
    }

    static void Shutdown(JNIEnv* env) {
        if (env) {
            ReleaseAll(env);
            ClearClickQueue(env);
        }

        int n = 0;
        Bind** all = AllPtrs(n);
        for (int i = 0; i < n; i++) {
            if (all[i]->obj && env) env->DeleteGlobalRef(all[i]->obj);
            all[i]->obj = nullptr;
            all[i]->pressed = nullptr;
            all[i]->overridden = false;
        }
        s_fPressed = nullptr;
        s_fPressTime = nullptr;
        s_ready = false;
    }

    // ---- Raw ----
    static bool Get(JNIEnv* env, Bind& b) {
        if (!b.obj || !b.pressed) return false;
        return env->GetBooleanField(b.obj, b.pressed) != 0;
    }

    static void Set(JNIEnv* env, Bind& b, bool down) {
        if (!b.obj || !b.pressed) return;
        env->SetBooleanField(b.obj, b.pressed, (jboolean)down);
        b.overridden = down;
        b.wanted = down;
    }

    static void Release(JNIEnv* env, Bind& b) {
        if (!b.obj || !b.pressed || !b.overridden) return;
        env->SetBooleanField(b.obj, b.pressed, (jboolean)false);
        b.overridden = false;
    }

    // Drop every key we are still holding. Safe at any time: keys the
    // player is physically holding are untouched, because we only
    // clear the ones we set ourselves.
    static void ReleaseAll(JNIEnv* env) {
        if (!env || !s_fPressed) return;
        int n = 0;
        Bind** all = AllPtrs(n);
        for (int i = 0; i < n; i++) Release(env, *all[i]);
    }

    // =============================================================
    // Click queue
    // =============================================================
    // Adds n queued press events, which the game turns into exactly
    // n calls to clickMouse() on its next tick.
    //
    // The counter is capped. If the game stalls, or we sit in a
    // menu, an uncapped counter keeps growing and then dumps the
    // whole backlog in a single tick: twenty attack packets with no
    // gap between them, which is the one thing that genuinely cannot
    // be produced by a hand.
    static constexpr int kMaxQueued = 4;

    static bool HasClickQueue() { return s_fPressTime != nullptr; }

    static int GetPressTime(JNIEnv* env, Bind& b) {
        if (!b.obj || !s_fPressTime) return 0;
        return env->GetIntField(b.obj, s_fPressTime);
    }

    static int AddPress(JNIEnv* env, Bind& b, int n) {
        if (!b.obj || !s_fPressTime || n <= 0) return 0;

        int cur = env->GetIntField(b.obj, s_fPressTime);
        if (cur >= kMaxQueued) return 0;

        int add = n;
        if (cur + add > kMaxQueued) add = kMaxQueued - cur;

        env->SetIntField(b.obj, s_fPressTime, cur + add);
        return add;
    }

    // Left click: swingItem plus attackEntity, the real path
    static int QueueAttack(JNIEnv* env, int n) { return AddPress(env, s_attack, n); }

    // Right click: item use, which is also how blocking starts
    static int QueueUse(JNIEnv* env, int n) { return AddPress(env, s_useItem, n); }

    static int PendingAttack(JNIEnv* env) { return GetPressTime(env, s_attack); }
    static int PendingUse(JNIEnv* env)    { return GetPressTime(env, s_useItem); }

    // Throw away anything still queued. Needed when a screen opens:
    // runTick stops consuming pressTime while a GUI is up, so the
    // backlog would fire all at once the moment it closes.
    static void ClearClickQueue(JNIEnv* env) {
        if (!env || !s_fPressTime) return;
        if (s_attack.obj)  env->SetIntField(s_attack.obj,  s_fPressTime, 0);
        if (s_useItem.obj) env->SetIntField(s_useItem.obj, s_fPressTime, 0);
    }

    // ---- Named ----
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

    // ---- Availability ----
    static bool HasMovement() { return s_forward.obj && s_back.obj; }
    static bool HasSneak()    { return s_sneak.obj != nullptr; }
    static bool HasJump()     { return s_jump.obj != nullptr; }
    static bool HasUseItem()  { return s_useItem.obj != nullptr; }
    static bool HasSprint()   { return s_sprint.obj != nullptr; }
    static bool HasAttack()   { return s_attack.obj != nullptr; }
};
