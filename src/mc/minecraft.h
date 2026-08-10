#pragma once
#include <jni.h>
#include <atomic>
#include <string>
#include <cmath>

#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"
#include "../util/log.h"

// =================================================================
// Minecraft — the game singleton and entity accessors
// =================================================================
// Field lookups go through JvmtiUtil, because GetFieldID needs an
// exact signature and Minecraft's are obfuscated (thePlayer is
// Lbew;, not anything guessable). JVMTI hands us the real signature
// so the same code works across Lunar builds.
//
// KeyBindings deliberately live in mc/keybinds.h, not here. That
// class tracks which keys we overrode so it can hand them back to
// the player cleanly.
// =================================================================

class Minecraft {
private:
    // Minecraft
    inline static jfieldID fThePlayer        = nullptr;
    inline static jfieldID fTheWorld         = nullptr;
    inline static jfieldID fGameSettings     = nullptr;
    inline static jfieldID fCurrentScreen    = nullptr;
    inline static jfieldID fPlayerController = nullptr;
    inline static jfieldID fTimer            = nullptr;

    // Entity
    inline static jfieldID fPosX = nullptr, fPosY = nullptr, fPosZ = nullptr;
    inline static jfieldID fPrevPosX = nullptr, fPrevPosY = nullptr, fPrevPosZ = nullptr;
    inline static jfieldID fMotionX = nullptr, fMotionY = nullptr, fMotionZ = nullptr;
    inline static jfieldID fYaw = nullptr, fPitch = nullptr;
    inline static jfieldID fOnGround = nullptr;
    inline static jfieldID fIsDead = nullptr;

    // EntityLivingBase
    inline static jfieldID fHurtTime = nullptr;

    // GameSettings
    inline static jfieldID fGamma = nullptr;

    // Timer
    inline static jfieldID fRenderPartialTicks = nullptr;

    inline static jmethodID mGetMinecraft = nullptr;

    // Sprinting and sneaking are DataWatcher flag bits, not plain
    // fields, so they have to be read through the accessors.
    inline static jmethodID mIsSprinting = nullptr;
    inline static jmethodID mIsSneaking  = nullptr;

    inline static jobject gMinecraft    = nullptr;
    inline static jobject gGameSettings = nullptr;

    inline static bool s_ready = false;

    // Our ImGui overlay is NOT a vanilla screen, so currentScreen is
    // null while it is open. Set from the window thread when the menu
    // toggles; read on the client thread. A plain atomic, no JNI, so
    // it is safe from either. See IsInGui below for why this exists.
    inline static std::atomic<bool> s_menuOpen{ false };

    static constexpr double kPi = 3.14159265358979323846;

public:
    // Vanilla eye height. Sneaking lowers it, and aiming from the
    // wrong height is a visible pitch error at close range, which is
    // exactly where every fight happens.
    static constexpr double kEyeHeight       = 1.62;
    static constexpr double kEyeHeightSneak  = 1.54;

    static bool IsReady() { return s_ready; }

    static bool Init(JNIEnv* env) {
        if (!ClassResolver::mcClass) return false;

        mGetMinecraft = JvmtiUtil::FindStaticMethod(env, ClassResolver::mcClass,
            { "func_71410_x", "getMinecraft" }, 0);

        auto mc = ClassResolver::mcClass;
        fThePlayer        = JvmtiUtil::FindField(env, mc, { "field_71439_g", "thePlayer" });
        fTheWorld         = JvmtiUtil::FindField(env, mc, { "field_71441_e", "theWorld" });
        fGameSettings     = JvmtiUtil::FindField(env, mc, { "field_71474_y", "gameSettings" });
        fCurrentScreen    = JvmtiUtil::FindField(env, mc, { "field_71462_r", "currentScreen" });
        fPlayerController = JvmtiUtil::FindField(env, mc, { "field_71442_b", "playerController" });
        fTimer            = JvmtiUtil::FindField(env, mc, { "field_71428_T", "timer" });

        if (ClassResolver::entity) {
            auto e = ClassResolver::entity;
            fPosX     = JvmtiUtil::FindField(env, e, { "field_70165_t", "posX" });
            fPosY     = JvmtiUtil::FindField(env, e, { "field_70163_u", "posY" });
            fPosZ     = JvmtiUtil::FindField(env, e, { "field_70161_v", "posZ" });
            fPrevPosX = JvmtiUtil::FindField(env, e, { "field_70169_q", "prevPosX" });
            fPrevPosY = JvmtiUtil::FindField(env, e, { "field_70167_r", "prevPosY" });
            fPrevPosZ = JvmtiUtil::FindField(env, e, { "field_70166_s", "prevPosZ" });
            fMotionX  = JvmtiUtil::FindField(env, e, { "field_70159_w", "motionX" });
            fMotionY  = JvmtiUtil::FindField(env, e, { "field_70181_x", "motionY" });
            fMotionZ  = JvmtiUtil::FindField(env, e, { "field_70179_y", "motionZ" });
            fYaw      = JvmtiUtil::FindField(env, e, { "field_70177_z", "rotationYaw" });
            fPitch    = JvmtiUtil::FindField(env, e, { "field_70125_A", "rotationPitch" });
            fOnGround = JvmtiUtil::FindField(env, e, { "field_70122_E", "onGround" });
            fIsDead   = JvmtiUtil::FindField(env, e, { "field_70128_L", "isDead" });

            mIsSprinting = JvmtiUtil::FindMethod(env, e, { "func_70051_ag", "isSprinting" }, 0);
            mIsSneaking  = JvmtiUtil::FindMethod(env, e, { "func_70093_af", "isSneaking" }, 0);
        }

        if (ClassResolver::entityLivingBase) {
            fHurtTime = JvmtiUtil::FindField(env, ClassResolver::entityLivingBase,
                { "field_70737_aN", "hurtTime" });
        }

        if (ClassResolver::gameSettings) {
            fGamma = JvmtiUtil::FindField(env, ClassResolver::gameSettings,
                { "field_74333_Y", "gammaSetting" });
        }

        if (ClassResolver::timerClass) {
            fRenderPartialTicks = JvmtiUtil::FindField(env, ClassResolver::timerClass,
                { "field_74281_c", "renderPartialTicks" });
        }

        // Cache the singleton so accessors stop allocating a local
        // ref on every call.
        jobject inst = FetchInstance(env);
        if (inst) {
            gMinecraft = env->NewGlobalRef(inst);
            env->DeleteLocalRef(inst);

            if (fGameSettings) {
                jobject gs = env->GetObjectField(gMinecraft, fGameSettings);
                if (gs) {
                    gGameSettings = env->NewGlobalRef(gs);
                    env->DeleteLocalRef(gs);
                }
            }
        }

        s_ready = (gMinecraft != nullptr && fThePlayer != nullptr);

        PH_LOG("[MC] ready=%d player=%p world=%p screen=%p posX=%p yaw=%p hurt=%p sprint=%p",
            (int)s_ready, (void*)fThePlayer, (void*)fTheWorld, (void*)fCurrentScreen,
            (void*)fPosX, (void*)fYaw, (void*)fHurtTime, (void*)mIsSprinting);

        if (!fCurrentScreen)
            PH_LOG("[MC] WARN: currentScreen unresolved, GUI checks disabled");

        return s_ready;
    }

    static void Shutdown(JNIEnv* env) {
        if (gMinecraft)    { env->DeleteGlobalRef(gMinecraft);    gMinecraft = nullptr; }
        if (gGameSettings) { env->DeleteGlobalRef(gGameSettings); gGameSettings = nullptr; }
        s_ready = false;
    }

    static jobject GetInstance(JNIEnv*)     { return gMinecraft; }
    static jobject GetGameSettings(JNIEnv*) { return gGameSettings; }

    static jobject GetPlayer(JNIEnv* env) {
        if (!gMinecraft || !fThePlayer) return nullptr;
        return env->GetObjectField(gMinecraft, fThePlayer);
    }

    static jobject GetWorld(JNIEnv* env) {
        if (!gMinecraft || !fTheWorld) return nullptr;
        return env->GetObjectField(gMinecraft, fTheWorld);
    }

    static jobject GetPlayerController(JNIEnv* env) {
        if (!gMinecraft || !fPlayerController) return nullptr;
        return env->GetObjectField(gMinecraft, fPlayerController);
    }

    // ---- Position and rotation ----
    static double GetPosX(JNIEnv* env, jobject e) { return fPosX ? env->GetDoubleField(e, fPosX) : 0.0; }
    static double GetPosY(JNIEnv* env, jobject e) { return fPosY ? env->GetDoubleField(e, fPosY) : 0.0; }
    static double GetPosZ(JNIEnv* env, jobject e) { return fPosZ ? env->GetDoubleField(e, fPosZ) : 0.0; }
    static double GetPrevPosX(JNIEnv* env, jobject e) { return fPrevPosX ? env->GetDoubleField(e, fPrevPosX) : GetPosX(env, e); }
    static double GetPrevPosY(JNIEnv* env, jobject e) { return fPrevPosY ? env->GetDoubleField(e, fPrevPosY) : GetPosY(env, e); }
    static double GetPrevPosZ(JNIEnv* env, jobject e) { return fPrevPosZ ? env->GetDoubleField(e, fPrevPosZ) : GetPosZ(env, e); }

    static float GetYaw(JNIEnv* env, jobject e)   { return fYaw   ? env->GetFloatField(e, fYaw)   : 0.f; }
    static float GetPitch(JNIEnv* env, jobject e) { return fPitch ? env->GetFloatField(e, fPitch) : 0.f; }
    static void  SetYaw(JNIEnv* env, jobject e, float v)   { if (fYaw)   env->SetFloatField(e, fYaw, v); }
    static void  SetPitch(JNIEnv* env, jobject e, float v) { if (fPitch) env->SetFloatField(e, fPitch, v); }

    static bool IsOnGround(JNIEnv* env, jobject e) { return fOnGround ? env->GetBooleanField(e, fOnGround) != 0 : false; }
    static bool IsDead(JNIEnv* env, jobject e)     { return fIsDead   ? env->GetBooleanField(e, fIsDead)   != 0 : false; }

    // ---- Motion ----
    static double GetMotionX(JNIEnv* env, jobject e) { return fMotionX ? env->GetDoubleField(e, fMotionX) : 0.0; }
    static double GetMotionY(JNIEnv* env, jobject e) { return fMotionY ? env->GetDoubleField(e, fMotionY) : 0.0; }
    static double GetMotionZ(JNIEnv* env, jobject e) { return fMotionZ ? env->GetDoubleField(e, fMotionZ) : 0.0; }
    static void SetMotionX(JNIEnv* env, jobject e, double v) { if (fMotionX) env->SetDoubleField(e, fMotionX, v); }
    static void SetMotionY(JNIEnv* env, jobject e, double v) { if (fMotionY) env->SetDoubleField(e, fMotionY, v); }
    static void SetMotionZ(JNIEnv* env, jobject e, double v) { if (fMotionZ) env->SetDoubleField(e, fMotionZ, v); }

    static int GetHurtTime(JNIEnv* env, jobject e) { return fHurtTime ? env->GetIntField(e, fHurtTime) : 0; }

    // ---- Flags ----
    // Read-only on purpose. Writing setSprinting is pointless: the
    // next onLivingUpdate recomputes it from the keybind. Modules
    // that want to change it go through KeyBinds instead.
    static bool HasSprintCheck() { return mIsSprinting != nullptr; }
    static bool HasSneakCheck()  { return mIsSneaking  != nullptr; }

    static bool IsSprinting(JNIEnv* env, jobject e) {
        if (!mIsSprinting || !e) return false;
        jboolean v = env->CallBooleanMethod(e, mIsSprinting);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return v != 0;
    }

    static bool IsSneaking(JNIEnv* env, jobject e) {
        if (!mIsSneaking || !e) return false;
        jboolean v = env->CallBooleanMethod(e, mIsSneaking);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return v != 0;
    }

    // Where this entity's eyes actually are, sneak included.
    static double GetEyeY(JNIEnv* env, jobject e) {
        if (!e) return 0.0;
        double h = IsSneaking(env, e) ? kEyeHeightSneak : kEyeHeight;
        return GetPosY(env, e) + h;
    }

    static float GetRenderPartialTicks(JNIEnv* env) {
        if (!gMinecraft || !fTimer || !fRenderPartialTicks) return 0.f;
        jobject t = env->GetObjectField(gMinecraft, fTimer);
        if (!t) return 0.f;
        float v = env->GetFloatField(t, fRenderPartialTicks);
        env->DeleteLocalRef(t);
        return v;
    }

    static void SetGamma(JNIEnv* env, float gamma) {
        if (!gGameSettings || !fGamma) return;
        env->SetFloatField(gGameSettings, fGamma, gamma);
    }

    static float GetGamma(JNIEnv* env) {
        if (!gGameSettings || !fGamma) return 1.f;
        return env->GetFloatField(gGameSettings, fGamma);
    }

    // ---- Math ----
    static double GetDistance(JNIEnv* env, jobject a, jobject b) {
        double dx = GetPosX(env, a) - GetPosX(env, b);
        double dy = GetPosY(env, a) - GetPosY(env, b);
        double dz = GetPosZ(env, a) - GetPosZ(env, b);
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    struct Rotation { float yaw; float pitch; };

    static Rotation GetRotationsToPos(JNIEnv* env, jobject from,
                                      double tx, double ty, double tz) {
        double dx = tx - GetPosX(env, from);
        double dz = tz - GetPosZ(env, from);
        double dy = ty - GetEyeY(env, from);
        double flat = std::sqrt(dx*dx + dz*dz);

        float yaw   = (float)(std::atan2(dz, dx) * 180.0 / kPi) - 90.0f;
        float pitch = (float)(-(std::atan2(dy, flat) * 180.0 / kPi));
        return { yaw, pitch };
    }

    static Rotation GetRotationsTo(JNIEnv* env, jobject from, jobject to) {
        return GetRotationsToPos(env, from,
            GetPosX(env, to), GetPosY(env, to) + 1.0, GetPosZ(env, to));
    }

    // ---- Menu state ----
    // The overlay opening is a window-thread event; releasing the
    // mouse grab and standing modules down is client-thread work, so
    // the window thread only flips this flag and the tick reacts.
    // Funnelled through ModuleManager::SetMenuOpen so the cursor,
    // this flag and the click scheduler can never disagree.
    static void SetMenuOpen(bool v) { s_menuOpen.store(v); }
    static bool IsMenuOpen()        { return s_menuOpen.load(); }

    // ---- State ----
    // A VANILLA screen only: currentScreen != null. The narrow
    // question "is a chest, inventory or pause menu open". This is
    // the one key reconcile must use, because the game clears every
    // key for a vanilla screen and we would be fighting it; it does
    // NOT do that for our own overlay. If the field could not be
    // resolved we return false so modules still run rather than
    // silently disabling the whole client.
    static bool IsVanillaGuiOpen(JNIEnv* env) {
        if (!gMinecraft || !fCurrentScreen) return false;
        jobject screen = env->GetObjectField(gMinecraft, fCurrentScreen);
        bool open = (screen != nullptr);
        if (screen) env->DeleteLocalRef(screen);
        return open;
    }

    // "Should a module keep its hands off the player right now?"
    // True for a vanilla screen OR our own overlay. Modules gate on
    // this, so opening the Phantom menu makes every one of them stand
    // down through exactly the same tested path a chest triggers,
    // without teaching fifteen files about the overlay.
    static bool IsInGui(JNIEnv* env) {
        if (s_menuOpen.load()) return true;
        return IsVanillaGuiOpen(env);
    }

    static bool InGame(JNIEnv* env) {
        if (!gMinecraft || !fThePlayer || !fTheWorld) return false;
        jobject p = env->GetObjectField(gMinecraft, fThePlayer);
        jobject w = env->GetObjectField(gMinecraft, fTheWorld);
        bool ok = (p != nullptr && w != nullptr);
        if (p) env->DeleteLocalRef(p);
        if (w) env->DeleteLocalRef(w);
        return ok;
    }

private:
    static jobject FetchInstance(JNIEnv* env) {
        if (!mGetMinecraft || !ClassResolver::mcClass) return nullptr;
        jobject o = env->CallStaticObjectMethod(ClassResolver::mcClass, mGetMinecraft);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return o;
    }
};
