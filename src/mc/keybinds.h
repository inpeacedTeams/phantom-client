#pragma once
#include <jni.h>
#include <jvmti.h>
#include <cstdio>
#include <cstring>

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
// -----------------------------------------------------------------
// RESTORING A KEY IS NOT THE SAME AS CLEARING IT
// -----------------------------------------------------------------
// This is the bug that killed movement after every hit.
//
// In 1.8 KeyBinding.pressed is only written when the keyboard fires
// an EVENT. Holding W down produces one press event and nothing
// else until you let go. So if we set pressed=false while the
// player is still physically holding W, the game never puts it back
// on its own: there is no event coming. The player is simply stuck
// standing still until they release W and press it again.
//
// Sprint reset does exactly that on every hit (W-tap sets forward
// to false for a tick), and the old Release() only ever wrote
// false. Worse, Set(false) marked the bind as "not overridden", so
// Release() saw nothing to undo and returned immediately. Forward
// stayed dead.
//
// Restoring correctly means writing back what is TRUE RIGHT NOW,
// not what we happened to save a tick ago, because the player may
// have let go in the meantime. So we ask the hardware through
// LWJGL, exactly like the game does:
//
//     keyCode >= 0  ->  Keyboard.isKeyDown(keyCode)
//     keyCode <  0  ->  Mouse.isButtonDown(keyCode + 100)
//
// The keycode is read fresh on every query rather than cached at
// inject time: rebinding a key in the options menu changes it, and
// a cached value would have us polling whatever W used to be.
//
// If LWJGL cannot be resolved we fall back to the value we saved
// when the override began, which is right in the common case.
//
// -----------------------------------------------------------------
// CLICK QUEUE
// -----------------------------------------------------------------
// A held key is a two-field state, and the two do different jobs:
//
//   pressed     is the key down right now
//   pressTime   how many press EVENTS are queued
//
//   Minecraft.runTick():
//       while (gameSettings.keyBindAttack.isPressed()) clickMouse();
//
//   KeyBinding.isPressed():
//       if (pressTime == 0) return false;
//       --pressTime; return true;
//
// So pressTime is a counter of clicks. Adding 1 makes the game call
// clickMouse() once on its next tick, running the real swingItem
// and attackEntity path. Holding `pressed` does not attack at all;
// it only mines.
// =================================================================

class KeyBinds {
private:
    struct Bind {
        jobject obj = nullptr;       // global ref to the KeyBinding
        jfieldID pressed = nullptr;

        bool overridden = false;     // we are driving this key
        bool value = false;          // what we forced it to
        bool saved = false;          // what it was before we touched it
    };

    inline static Bind s_forward, s_back, s_left, s_right;
    inline static Bind s_jump, s_sneak, s_sprint;
    inline static Bind s_useItem, s_attack;

    inline static jfieldID s_fPressed   = nullptr;
    inline static jfieldID s_fPressTime = nullptr;
    inline static jfieldID s_fKeyCode   = nullptr;
    inline static bool s_ready = false;

    // ---- LWJGL, for asking what the hardware is actually doing ----
    inline static jclass    s_keyboardClass = nullptr;
    inline static jmethodID s_isKeyDown     = nullptr;
    inline static jclass    s_mouseClass    = nullptr;
    inline static jmethodID s_isButtonDown  = nullptr;
    inline static bool s_lwjglTried = false;

    static Bind** AllPtrs(int& count) {
        static Bind* table[] = {
            &s_forward, &s_back, &s_left, &s_right,
            &s_jump, &s_sneak, &s_sprint, &s_useItem, &s_attack
        };
        count = (int)(sizeof(table) / sizeof(table[0]));
        return table;
    }

    // Lunar's classloader hides everything from env->FindClass, so
    // LWJGL has to be located the same way Minecraft's classes are.
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

    static void ResolveLwjgl(JNIEnv* env) {
        if (s_lwjglTried) return;
        s_lwjglTried = true;

        s_keyboardClass = FindBySignature(env, "Lorg/lwjgl/input/Keyboard;");
        if (s_keyboardClass) {
            s_isKeyDown = env->GetStaticMethodID(s_keyboardClass, "isKeyDown", "(I)Z");
            if (env->ExceptionCheck()) { env->ExceptionClear(); s_isKeyDown = nullptr; }
        }

        s_mouseClass = FindBySignature(env, "Lorg/lwjgl/input/Mouse;");
        if (s_mouseClass) {
            s_isButtonDown = env->GetStaticMethodID(s_mouseClass, "isButtonDown", "(I)Z");
            if (env->ExceptionCheck()) { env->ExceptionClear(); s_isButtonDown = nullptr; }
        }

        printf("[KeyBinds] LWJGL keyboard=%p mouse=%p\n",
            (void*)s_isKeyDown, (void*)s_isButtonDown);

        if (!s_isKeyDown) {
            printf("[KeyBinds] WARN: cannot read the hardware, "
                   "key restore falls back to the saved value\n");
        }
    }

    // Is this key physically down right now? Returns false if we
    // have no way to find out, so the caller keeps its saved value.
    //
    // The keycode is read live. Caching it at inject time meant that
    // rebinding forward in the options menu left us polling the old
    // key forever, and every restore would then write whatever that
    // dead key happened to be doing.
    static bool PhysicalState(JNIEnv* env, Bind& b, bool* out) {
        if (!b.obj || !s_fKeyCode) return false;

        jint code = env->GetIntField(b.obj, s_fKeyCode);

        if (code < 0) {
            // LWJGL convention: negative means a mouse button,
            // offset by 100.
            if (!s_isButtonDown) return false;
            jboolean v = env->CallStaticBooleanMethod(
                s_mouseClass, s_isButtonDown, (jint)(code + 100));
            if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
            *out = (v != 0);
            return true;
        }

        if (code == 0) return false;   // unbound
        if (!s_isKeyDown) return false;

        jboolean v = env->CallStaticBooleanMethod(s_keyboardClass, s_isKeyDown, code);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        *out = (v != 0);
        return true;
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

        if (!s_fPressed || !s_fPressTime || !s_fKeyCode) {
            jclass cls = env->GetObjectClass(kb);
            if (!s_fPressed)
                s_fPressed = JvmtiUtil::FindField(env, cls, { "field_74513_e", "pressed" });
            // Only these two names. field_74512_d is keyCode, and
            // accepting it as a fallback meant a failed pressTime
            // lookup would silently start writing click counts into
            // the key code and unbind the player's controls.
            if (!s_fPressTime)
                s_fPressTime = JvmtiUtil::FindField(env, cls, { "field_151474_i", "pressTime" });
            if (!s_fKeyCode)
                s_fKeyCode = JvmtiUtil::FindField(env, cls, { "field_74512_d", "keyCode" });
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
            ResolveLwjgl(env);

            printf("[KeyBinds] resolved fwd=%p sneak=%p jump=%p use=%p attack=%p\n",
                (void*)s_forward.obj, (void*)s_sneak.obj, (void*)s_jump.obj,
                (void*)s_useItem.obj, (void*)s_attack.obj);
            printf("[KeyBinds] pressed=%p pressTime=%p keyCode=%p\n",
                (void*)s_fPressed, (void*)s_fPressTime, (void*)s_fKeyCode);

            if (!s_fPressTime)
                printf("[KeyBinds] WARN: pressTime unresolved, native clicking is off\n");
            if (!s_fKeyCode)
                printf("[KeyBinds] WARN: keyCode unresolved, cannot poll the hardware\n");
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

        if (env) {
            if (s_keyboardClass) env->DeleteGlobalRef(s_keyboardClass);
            if (s_mouseClass)    env->DeleteGlobalRef(s_mouseClass);
        }
        s_keyboardClass = nullptr;
        s_mouseClass = nullptr;
        s_isKeyDown = nullptr;
        s_isButtonDown = nullptr;
        s_lwjglTried = false;

        s_fPressed = nullptr;
        s_fPressTime = nullptr;
        s_fKeyCode = nullptr;
        s_ready = false;
    }

    // ---- Raw ----
    static bool Get(JNIEnv* env, Bind& b) {
        if (!b.obj || !b.pressed) return false;
        return env->GetBooleanField(b.obj, b.pressed) != 0;
    }

    // Force a key to a state. Remembers what it was, so Release can
    // put it back. Works in both directions: forcing a key UP is
    // just as much an override as forcing it DOWN, which is the
    // distinction the old version missed.
    static void Set(JNIEnv* env, Bind& b, bool down) {
        if (!b.obj || !b.pressed) return;

        if (!b.overridden) {
            b.saved = env->GetBooleanField(b.obj, b.pressed) != 0;
            b.overridden = true;
        }
        b.value = down;
        env->SetBooleanField(b.obj, b.pressed, (jboolean)down);
    }

    // Hand the key back to the player. Restores what the hardware
    // says right now, because they may have let go while we held it.
    static void Release(JNIEnv* env, Bind& b) {
        if (!b.obj || !b.pressed || !b.overridden) return;
        b.overridden = false;

        bool restore = b.saved;
        bool live = false;
        if (PhysicalState(env, b, &live)) restore = live;

        env->SetBooleanField(b.obj, b.pressed, (jboolean)restore);
    }

    // Drop every key we are still driving. Safe at any time: keys we
    // never touched are left exactly as they are.
    static void ReleaseAll(JNIEnv* env) {
        if (!env || !s_fPressed) return;
        int n = 0;
        Bind** all = AllPtrs(n);
        for (int i = 0; i < n; i++) Release(env, *all[i]);
    }

    // Is anything still overridden? Useful as a stuck-key canary.
    static int HeldCount() {
        int n = 0, held = 0;
        Bind** all = AllPtrs(n);
        for (int i = 0; i < n; i++) if (all[i]->overridden) held++;
        return held;
    }

    // =============================================================
    // Click queue
    // =============================================================
    // The counter is capped. If the game stalls, or we sit in a
    // menu, an uncapped counter keeps growing and then dumps the
    // whole backlog in a single tick: a burst of attack packets with
    // no gap between them, which genuinely cannot come from a hand.
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

    // What the player is physically doing, ignoring our overrides.
    // Modules that need to know whether the human is still holding
    // a key should ask these, not the Get* pair.
    static bool Phys(JNIEnv* e, Bind& b) {
        bool v = false;
        if (PhysicalState(e, b, &v)) return v;
        return b.overridden ? b.saved : Get(e, b);
    }

    static bool PhysForward(JNIEnv* e) { return Phys(e, s_forward); }
    static bool PhysBack(JNIEnv* e)    { return Phys(e, s_back); }
    static bool PhysLeft(JNIEnv* e)    { return Phys(e, s_left); }
    static bool PhysRight(JNIEnv* e)   { return Phys(e, s_right); }
    static bool PhysSneak(JNIEnv* e)   { return Phys(e, s_sneak); }
    static bool PhysJump(JNIEnv* e)    { return Phys(e, s_jump); }

    static bool PhysMoving(JNIEnv* e) {
        return PhysForward(e) || PhysBack(e) || PhysLeft(e) || PhysRight(e);
    }

    static void ReleaseForward(JNIEnv* e) { Release(e, s_forward); }
    static void ReleaseBack(JNIEnv* e)    { Release(e, s_back); }
    static void ReleaseLeft(JNIEnv* e)    { Release(e, s_left); }
    static void ReleaseRight(JNIEnv* e)   { Release(e, s_right); }
    static void ReleaseJump(JNIEnv* e)    { Release(e, s_jump); }
    static void ReleaseSneak(JNIEnv* e)   { Release(e, s_sneak); }
    static void ReleaseSprint(JNIEnv* e)  { Release(e, s_sprint); }
    static void ReleaseUseItem(JNIEnv* e) { Release(e, s_useItem); }
    static void ReleaseAttack(JNIEnv* e)  { Release(e, s_attack); }

    // ---- Availability ----
    static bool HasMovement()     { return s_forward.obj && s_back.obj; }
    static bool HasSneak()        { return s_sneak.obj != nullptr; }
    static bool HasJump()         { return s_jump.obj != nullptr; }
    static bool HasUseItem()      { return s_useItem.obj != nullptr; }
    static bool HasSprint()       { return s_sprint.obj != nullptr; }
    static bool HasAttack()       { return s_attack.obj != nullptr; }
    static bool CanReadHardware() { return s_isKeyDown != nullptr; }
};
