#pragma once
#include <jni.h>

#include "minecraft.h"
#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"
#include "../util/log.h"

// =================================================================
// MouseControl
// =================================================================
// WHY THE MENU CURSOR DID NOT WORK
//
// Minecraft grabs the mouse while you are in a world. A grabbed
// mouse is not a pointer: LWJGL hides it, locks it to the centre of
// the window and reports raw deltas, which the game feeds straight
// into the camera.
//
// Drawing an ImGui cursor on top of that changes nothing. The
// pointer is pinned to the middle of the screen, so it never
// reaches a switch, and every attempt to move it spins your view
// instead. That is the whole bug: it was never a rendering problem.
//
// THE FIX IS TO ASK THE GAME, NOT THE OS
//
// Calling Mouse.setGrabbed(false) ourselves does not hold. The
// game re-grabs on the very next tick, because Minecraft.runTick
// re-asserts the grab whenever inGameHasFocus is true.
//
// So we use the game's own pair:
//
//   setIngameNotInFocus()  ungrab, show the cursor, stop feeding
//                          mouse movement to the camera. This is
//                          exactly what opening a chest does.
//   setIngameFocus()       grab again, hide the cursor.
//
// Because the game is doing it, everything downstream agrees:
// no camera drift, no fighting over the cursor, and the moment the
// menu closes you are back in the world with the view untouched.
//
// The state is re-asserted every tick while the menu is open. A
// click that slips through to the game would otherwise call
// setIngameFocus and snatch the cursor back mid-session.
//
// HANDS OFF WHEN A VANILLA SCREEN IS OPEN
//
// setIngameFocus does more than grab the mouse. It also calls
// displayGuiScreen(null), which closes whatever screen is open.
// So opening the menu on top of your inventory and closing it again
// used to close the inventory too, and reset the left-click cooldown
// while it was at it.
//
// When a vanilla screen is open the cursor is already free, so there
// is nothing for us to do. We take no ownership and give none back.
// =================================================================

class MouseControl {
private:
    inline static jmethodID s_setNotInFocus = nullptr;   // ungrab
    inline static jmethodID s_setFocus      = nullptr;   // grab
    inline static jfieldID  s_fHasFocus     = nullptr;   // inGameHasFocus

    inline static bool s_resolved = false;
    inline static bool s_usable   = false;

    // Whether WE are the reason the cursor is free. Never set while
    // a vanilla screen is doing it for us.
    inline static bool s_released = false;

public:
    static bool IsUsable()   { return s_usable; }
    static bool IsReleased() { return s_released; }

    static bool Init(JNIEnv* env) {
        if (s_resolved) return s_usable;
        if (!ClassResolver::mcClass) return false;

        jclass mc = ClassResolver::mcClass;

        s_setNotInFocus = JvmtiUtil::FindMethod(env, mc,
            { "func_71364_i", "setIngameNotInFocus" }, 0);
        s_setFocus = JvmtiUtil::FindMethod(env, mc,
            { "func_71381_h", "setIngameFocus" }, 0);
        s_fHasFocus = JvmtiUtil::FindField(env, mc,
            { "field_71415_G", "inGameHasFocus" });

        s_usable = (s_setNotInFocus != nullptr && s_setFocus != nullptr);
        s_resolved = true;

        PH_LOG("[Mouse] ungrab=%p grab=%p focusFlag=%p usable=%d",
            (void*)s_setNotInFocus, (void*)s_setFocus,
            (void*)s_fHasFocus, (int)s_usable);

        if (!s_usable)
            PH_LOG("[Mouse] WARN: cannot release the grab, the menu cursor "
                   "will be stuck to the centre");

        return s_usable;
    }

    static void Shutdown() {
        s_resolved = false;
        s_usable = false;
        s_released = false;
    }

    // Does the game currently believe it owns the mouse?
    static bool GameHasFocus(JNIEnv* env) {
        jobject inst = Minecraft::GetInstance(env);
        if (!inst || !s_fHasFocus) return true;   // assume yes
        return env->GetBooleanField(inst, s_fHasFocus) != 0;
    }

    // -------------------------------------------------------------
    // Call every tick with whether the menu is open.
    //
    // Idempotent by design: it only calls into the game when the
    // state actually needs to change, but it DOES re-assert the
    // released state, because the game will happily re-grab behind
    // our back on any stray click.
    // -------------------------------------------------------------
    static void Apply(JNIEnv* env, bool menuOpen) {
        if (!Init(env)) return;

        jobject inst = Minecraft::GetInstance(env);
        if (!inst) return;

        // Only meaningful in a world. At the main menu the cursor is
        // already free and grabbing would be wrong.
        if (!Minecraft::InGame(env)) {
            s_released = false;
            return;
        }

        // A chest, the inventory, chat, the pause screen: the game
        // already owns this situation and handing focus back would
        // close the screen out from under the player.
        if (Minecraft::IsInGui(env)) {
            s_released = false;
            return;
        }

        if (menuOpen) {
            // Re-assert. Cheap, and it survives the game deciding to
            // take the mouse back on its own.
            if (!s_released || GameHasFocus(env)) {
                env->CallVoidMethod(inst, s_setNotInFocus);
                if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
                s_released = true;
            }
            return;
        }

        if (s_released) {
            // Hand the mouse back exactly as the game would after
            // closing a chest.
            env->CallVoidMethod(inst, s_setFocus);
            if (env->ExceptionCheck()) env->ExceptionClear();
            s_released = false;
        }
    }

    // Eject path: never leave the player without a grabbed mouse.
    static void ForceRestore(JNIEnv* env) {
        if (!s_usable || !s_released) return;
        jobject inst = Minecraft::GetInstance(env);
        if (!inst) { s_released = false; return; }

        // Same reasoning as Apply: if a screen is open the cursor is
        // meant to be free, and grabbing would close the screen.
        if (Minecraft::InGame(env) && !Minecraft::IsInGui(env)) {
            env->CallVoidMethod(inst, s_setFocus);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        s_released = false;
    }
};
