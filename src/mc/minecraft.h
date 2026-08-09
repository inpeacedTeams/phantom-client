#pragma once
#include <jni.h>
#include <cstdio>
#include <string>
#include <cmath>

#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"

// =================================================================
// Minecraft — JNI wrapper around the game singleton and entities
// =================================================================
// All field lookups go through JvmtiUtil so obfuscated signatures
// do not matter. Candidate names are listed SRG first, then MCP.
// =================================================================

class Minecraft {
private:
    // Minecraft fields
    inline static jfieldID fThePlayer        = nullptr;
    inline static jfieldID fTheWorld         = nullptr;
    inline static jfieldID fGameSettings     = nullptr;
    inline static jfieldID fCurrentScreen    = nullptr;
    inline static jfieldID fPlayerController = nullptr;
    inline static jfieldID fTimer            = nullptr;

    // Entity fields
    inline static jfieldID fPosX = nullptr, fPosY = nullptr, fPosZ = nullptr;
    inline static jfieldID fPrevPosX = nullptr, fPrevPosY = nullptr, fPrevPosZ = nullptr;
    inline static jfieldID fMotionX = nullptr, fMotionY = nullptr, fMotionZ = nullptr;
    inline static jfieldID fYaw = nullptr, fPitch = nullptr;
    inline static jfieldID fOnGround = nullptr;
    inline static jfieldID fIsDead = nullptr;

    // EntityLivingBase
    inline static jfieldID fHurtTime = nullptr;
    inline static jfieldID fHealth   = nullptr;

    // GameSettings
    inline static jfieldID fGamma          = nullptr;
    inline static jfieldID fKeyBindUseItem = nullptr;

    // Timer
    inline static jfieldID fRenderPartialTicks = nullptr;

    // Static accessor
    inline static jmethodID mGetMinecraft = nullptr;

    // Cached global refs
    inline static jobject gMinecraft   = nullptr;
    inline static jobject gGameSettings = nullptr;

    inline static bool s_ready = false;

public:
    static bool IsReady() { return s_ready; }

    static bool Init(JNIEnv* env) {
        if (!ClassResolver::mcClass) return false;

        // Minecraft.getMinecraft() / func_71410_x()
        mGetMinecraft = JvmtiUtil::FindStaticMethod(env, ClassResolver::mcClass,
            { "func_71410_x", "getMinecraft", "A" }, 0);

        auto mc = ClassResolver::mcClass;
        fThePlayer        = JvmtiUtil::FindField(env, mc, { "field_71439_g", "thePlayer" });
        fTheWorld         = JvmtiUtil::FindField(env, mc, { "field_71441_e", "theWorld" });
        fGameSettings     = JvmtiUtil::FindField(env, mc, { "field_71474_y", "gameSettings" });
        // THE FIX: currentScreen was declared but never resolved, so
        // IsInGui() always returned true and every module bailed out.
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
        }

        if (ClassResolver::entityLivingBase) {
            auto lb = ClassResolver::entityLivingBase;
            fHurtTime = JvmtiUtil::FindField(env, lb, { "field_70737_aN", "hurtTime" });
            // health lives in the DataWatcher in 1.8; the mirrored
            // field is only present on some mappings
            fHealth   = JvmtiUtil::FindField(env, lb, { "field_70760_ar", "health" });
        }

        if (ClassResolver::gameSettings) {
            auto gs = ClassResolver::gameSettings;
            fGamma          = JvmtiUtil::FindField(env, gs, { "field_74333_Y", "gammaSetting" });
            fKeyBindUseItem = JvmtiUtil::FindField(env, gs, { "field_74313_G", "keyBindUseItem" });
        }

        if (ClassResolver::timerClass) {
            fRenderPartialTicks = JvmtiUtil::FindField(env, ClassResolver::timerClass,
                { "field_74281_c", "renderPartialTicks" });
        }

        // Cache the singleton as a global ref so we stop allocating a
        // local ref on every single accessor call.
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

        printf("[MC] ready=%d player=%p world=%p screen=%p posX=%p yaw=%p hurt=%p\n",
            (int)s_ready, (void*)fThePlayer, (void*)fTheWorld, (void*)fCurrentScreen,
            (void*)fPosX, (void*)fYaw, (void*)fHurtTime);

        if (!fCurrentScreen)
            printf("[MC] WARN: currentScreen unresolved, GUI checks disabled\n");

        return s_ready;
    }

    static void Shutdown(JNIEnv* env) {
        if (gMinecraft)    { env->DeleteGlobalRef(gMinecraft);    gMinecraft = nullptr; }
        if (gGameSettings) { env->DeleteGlobalRef(gGameSettings); gGameSettings = nullptr; }
        s_ready = false;
    }

    static jobject GetInstance(JNIEnv*) { return gMinecraft; }
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

    static jobject GetKeyBindUseItem(JNIEnv* env) {
        if (!gGameSettings || !fKeyBindUseItem) return nullptr;
        return env->GetObjectField(gGameSettings, fKeyBindUseItem);
    }

    // ---- Position / rotation ----
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

    static bool IsOnGround(JNIEnv* env, jobject e) { return fOnGround ? env->GetBooleanField(e, fOnGround) : false; }
    static bool IsDead(JNIEnv* env, jobject e)     { return fIsDead   ? env->GetBooleanField(e, fIsDead)   : false; }

    // ---- Motion ----
    static double GetMotionX(JNIEnv* env, jobject e) { return fMotionX ? env->GetDoubleField(e, fMotionX) : 0.0; }
    static double GetMotionY(JNIEnv* env, jobject e) { return fMotionY ? env->GetDoubleField(e, fMotionY) : 0.0; }
    static double GetMotionZ(JNIEnv* env, jobject e) { return fMotionZ ? env->GetDoubleField(e, fMotionZ) : 0.0; }
    static void SetMotionX(JNIEnv* env, jobject e, double v) { if (fMotionX) env->SetDoubleField(e, fMotionX, v); }
    static void SetMotionY(JNIEnv* env, jobject e, double v) { if (fMotionY) env->SetDoubleField(e, fMotionY, v); }
    static void SetMotionZ(JNIEnv* env, jobject e, double v) { if (fMotionZ) env->SetDoubleField(e, fMotionZ, v); }

    static int GetHurtTime(JNIEnv* env, jobject e) { return fHurtTime ? env->GetIntField(e, fHurtTime) : 0; }

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

    // ---- Distance / rotation math ----
    static double GetDistance(JNIEnv* env, jobject a, jobject b) {
        double dx = GetPosX(env, a) - GetPosX(env, b);
        double dy = GetPosY(env, a) - GetPosY(env, b);
        double dz = GetPosZ(env, a) - GetPosZ(env, b);
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    struct Rotation { float yaw; float pitch; };

    static Rotation GetRotationsTo(JNIEnv* env, jobject from, jobject to) {
        double dx = GetPosX(env, to) - GetPosX(env, from);
        double dz = GetPosZ(env, to) - GetPosZ(env, from);
        // Aim at chest height, not feet
        double dy = (GetPosY(env, to) + 1.0) - (GetPosY(env, from) + 1.62);
        double flat = std::sqrt(dx*dx + dz*dz);

        float yaw   = (float)(std::atan2(dz, dx) * 180.0 / 3.14159265358979) - 90.0f;
        float pitch = (float)(-(std::atan2(dy, flat) * 180.0 / 3.14159265358979));
        return { yaw, pitch };
    }

    static Rotation GetRotationsToPos(JNIEnv* env, jobject from,
                                      double tx, double ty, double tz) {
        double dx = tx - GetPosX(env, from);
        double dz = tz - GetPosZ(env, from);
        double dy = ty - (GetPosY(env, from) + 1.62);
        double flat = std::sqrt(dx*dx + dz*dz);

        float yaw   = (float)(std::atan2(dz, dx) * 180.0 / 3.14159265358979) - 90.0f;
        float pitch = (float)(-(std::atan2(dy, flat) * 180.0 / 3.14159265358979));
        return { yaw, pitch };
    }

    // ---- GUI state ----
    // Returns true only when a screen is actually open. If the field
    // could not be resolved we return FALSE so modules still run,
    // rather than silently disabling the entire client.
    static bool IsInGui(JNIEnv* env) {
        if (!gMinecraft || !fCurrentScreen) return false;
        jobject screen = env->GetObjectField(gMinecraft, fCurrentScreen);
        bool open = (screen != nullptr);
        if (screen) env->DeleteLocalRef(screen);
        return open;
    }

    // True when we are actually in a world with a player
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
