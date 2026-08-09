#pragma once
#include <jni.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

#include "../jni/class_resolver.h"
#include "../mappings/mcp189.h"

// Helper: try field name with SRG fallback
static jfieldID FindFieldMulti(JNIEnv* env, jclass cls, const char* name1, const char* name2, const char* sig) {
    jfieldID f = env->GetFieldID(cls, name1, sig);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        f = env->GetFieldID(cls, name2, sig);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return nullptr;
        }
    }
    return f;
}

class Minecraft {
private:
    // Cached field IDs
    inline static jfieldID fThePlayer     = nullptr;
    inline static jfieldID fTheWorld      = nullptr;
    inline static jfieldID fGameSettings  = nullptr;
    inline static jfieldID fTimer         = nullptr;
    inline static jfieldID fPlayerController = nullptr;
    inline static jfieldID fCurrentScreen = nullptr;

    // Entity fields
    inline static jfieldID fPosX = nullptr, fPosY = nullptr, fPosZ = nullptr;
    inline static jfieldID fPrevPosX = nullptr, fPrevPosY = nullptr, fPrevPosZ = nullptr;
    inline static jfieldID fMotionX = nullptr, fMotionY = nullptr, fMotionZ = nullptr;
    inline static jfieldID fYaw = nullptr, fPitch = nullptr;
    inline static jfieldID fOnGround = nullptr;
    inline static jfieldID fIsDead = nullptr;
    inline static jfieldID fHurtTime = nullptr;
    inline static jfieldID fHurtResistantTime = nullptr;

    // GameSettings
    inline static jfieldID fGamma = nullptr;

    // Timer
    inline static jfieldID fRenderPartialTicks = nullptr;
    inline static jfieldID fTimerSpeed = nullptr;

    // World
    inline static jfieldID fPlayerEntities = nullptr;

    // RenderManager
    inline static jfieldID fRenderPosX = nullptr;
    inline static jfieldID fRenderPosY = nullptr;
    inline static jfieldID fRenderPosZ = nullptr;

    // Minecraft instance (static, singleton)
    inline static jobject mcInstance = nullptr;

    // Method to get instance
    inline static jmethodID mGetMinecraft = nullptr;

public:
    static bool Init(JNIEnv* env) {
        if (!ClassResolver::mcClass) return false;

        // Get Minecraft.getMinecraft() static method
        mGetMinecraft = env->GetStaticMethodID(
            ClassResolver::mcClass,
            MCP::Minecraft::getMinecraft_srg,
            MCP::Minecraft::getMinecraft_sig
        );
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            // Try MCP name
            mGetMinecraft = env->GetStaticMethodID(
                ClassResolver::mcClass, "getMinecraft",
                MCP::Minecraft::getMinecraft_sig
            );
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // Resolve fields (try SRG then MCP)
        auto mc = ClassResolver::mcClass;
        fThePlayer = FindFieldMulti(env, mc, MCP::Minecraft::thePlayer_srg, MCP::Minecraft::thePlayer_mcp, "Ljava/lang/Object;");
        fTheWorld  = FindFieldMulti(env, mc, MCP::Minecraft::theWorld_srg, MCP::Minecraft::theWorld_mcp, "Ljava/lang/Object;");

        // Entity fields
        if (ClassResolver::entity) {
            auto e = ClassResolver::entity;
            fPosX = FindFieldMulti(env, e, MCP::Entity::posX_srg, MCP::Entity::posX_mcp, "D");
            fPosY = FindFieldMulti(env, e, MCP::Entity::posY_srg, MCP::Entity::posY_mcp, "D");
            fPosZ = FindFieldMulti(env, e, MCP::Entity::posZ_srg, MCP::Entity::posZ_mcp, "D");
            fMotionX = FindFieldMulti(env, e, MCP::Entity::motionX_srg, MCP::Entity::motionX_mcp, "D");
            fMotionY = FindFieldMulti(env, e, MCP::Entity::motionY_srg, MCP::Entity::motionY_mcp, "D");
            fMotionZ = FindFieldMulti(env, e, MCP::Entity::motionZ_srg, MCP::Entity::motionZ_mcp, "D");
            fYaw   = FindFieldMulti(env, e, MCP::Entity::rotationYaw_srg, MCP::Entity::rotationYaw_mcp, "F");
            fPitch = FindFieldMulti(env, e, MCP::Entity::rotationPitch_srg, MCP::Entity::rotationPitch_mcp, "F");
            fOnGround = FindFieldMulti(env, e, MCP::Entity::onGround_srg, MCP::Entity::onGround_mcp, "Z");

            fPrevPosX = env->GetFieldID(e, MCP::Entity::prevPosX_srg, "D");
            if (env->ExceptionCheck()) env->ExceptionClear();
            fPrevPosY = env->GetFieldID(e, MCP::Entity::prevPosY_srg, "D");
            if (env->ExceptionCheck()) env->ExceptionClear();
            fPrevPosZ = env->GetFieldID(e, MCP::Entity::prevPosZ_srg, "D");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // EntityLivingBase hurtTime
        if (ClassResolver::entityLivingBase) {
            fHurtTime = FindFieldMulti(env, ClassResolver::entityLivingBase,
                MCP::EntityLivingBase::hurtTime_srg, MCP::EntityLivingBase::hurtTime_mcp, "I");
        }

        // GameSettings gamma
        if (ClassResolver::gameSettings) {
            fGamma = FindFieldMulti(env, ClassResolver::gameSettings,
                MCP::GameSettings::gammaSetting_srg, MCP::GameSettings::gammaSetting_mcp, "F");
        }

        printf("[MC] Fields resolved: posX=%p, posY=%p, yaw=%p, thePlayer=%p\n",
            fPosX, fPosY, fYaw, fThePlayer);

        return fThePlayer != nullptr;
    }

    // Get Minecraft singleton instance
    static jobject GetInstance(JNIEnv* env) {
        if (mGetMinecraft && ClassResolver::mcClass) {
            return env->CallStaticObjectMethod(ClassResolver::mcClass, mGetMinecraft);
        }
        return nullptr;
    }

    // Get thePlayer
    static jobject GetPlayer(JNIEnv* env) {
        jobject mc = GetInstance(env);
        if (!mc || !fThePlayer) return nullptr;
        return env->GetObjectField(mc, fThePlayer);
    }

    // Get theWorld
    static jobject GetWorld(JNIEnv* env) {
        jobject mc = GetInstance(env);
        if (!mc || !fTheWorld) return nullptr;
        return env->GetObjectField(mc, fTheWorld);
    }

    // ===== Entity position/rotation =====
    static double GetPosX(JNIEnv* env, jobject entity) {
        return fPosX ? env->GetDoubleField(entity, fPosX) : 0.0;
    }
    static double GetPosY(JNIEnv* env, jobject entity) {
        return fPosY ? env->GetDoubleField(entity, fPosY) : 0.0;
    }
    static double GetPosZ(JNIEnv* env, jobject entity) {
        return fPosZ ? env->GetDoubleField(entity, fPosZ) : 0.0;
    }
    static float GetYaw(JNIEnv* env, jobject entity) {
        return fYaw ? env->GetFloatField(entity, fYaw) : 0.f;
    }
    static float GetPitch(JNIEnv* env, jobject entity) {
        return fPitch ? env->GetFloatField(entity, fPitch) : 0.f;
    }
    static void SetYaw(JNIEnv* env, jobject entity, float yaw) {
        if (fYaw) env->SetFloatField(entity, fYaw, yaw);
    }
    static void SetPitch(JNIEnv* env, jobject entity, float pitch) {
        if (fPitch) env->SetFloatField(entity, fPitch, pitch);
    }
    static bool IsOnGround(JNIEnv* env, jobject entity) {
        return fOnGround ? env->GetBooleanField(entity, fOnGround) : false;
    }

    // ===== Motion =====
    static double GetMotionX(JNIEnv* env, jobject entity) {
        return fMotionX ? env->GetDoubleField(entity, fMotionX) : 0.0;
    }
    static double GetMotionY(JNIEnv* env, jobject entity) {
        return fMotionY ? env->GetDoubleField(entity, fMotionY) : 0.0;
    }
    static double GetMotionZ(JNIEnv* env, jobject entity) {
        return fMotionZ ? env->GetDoubleField(entity, fMotionZ) : 0.0;
    }
    static void SetMotionX(JNIEnv* env, jobject entity, double v) {
        if (fMotionX) env->SetDoubleField(entity, fMotionX, v);
    }
    static void SetMotionY(JNIEnv* env, jobject entity, double v) {
        if (fMotionY) env->SetDoubleField(entity, fMotionY, v);
    }
    static void SetMotionZ(JNIEnv* env, jobject entity, double v) {
        if (fMotionZ) env->SetDoubleField(entity, fMotionZ, v);
    }

    // ===== Hurt time =====
    static int GetHurtTime(JNIEnv* env, jobject entity) {
        return fHurtTime ? env->GetIntField(entity, fHurtTime) : 0;
    }

    // ===== GameSettings gamma =====
    static void SetGamma(JNIEnv* env, float gamma) {
        jobject mc = GetInstance(env);
        if (!mc || !fGamma) return;
        // Get gameSettings object first
        jfieldID gsField = FindFieldMulti(env, ClassResolver::mcClass,
            MCP::Minecraft::gameSettings_srg, MCP::Minecraft::gameSettings_mcp,
            "Ljava/lang/Object;");
        if (!gsField) return;
        jobject gs = env->GetObjectField(mc, gsField);
        if (!gs) return;
        env->SetFloatField(gs, fGamma, gamma);
    }

    // ===== Distance =====
    static double GetDistance(JNIEnv* env, jobject e1, jobject e2) {
        double dx = GetPosX(env, e1) - GetPosX(env, e2);
        double dy = GetPosY(env, e1) - GetPosY(env, e2);
        double dz = GetPosZ(env, e1) - GetPosZ(env, e2);
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    // ===== Rotation calculation =====
    struct Rotation {
        float yaw;
        float pitch;
    };

    static Rotation GetRotationsTo(JNIEnv* env, jobject from, jobject to) {
        double dx = GetPosX(env, to) - GetPosX(env, from);
        double dy = (GetPosY(env, to) + 1.62) - (GetPosY(env, from) + 1.62); // eye height
        double dz = GetPosZ(env, to) - GetPosZ(env, from);
        double dist = std::sqrt(dx*dx + dz*dz);

        float yaw   = (float)(std::atan2(dz, dx) * 180.0 / 3.14159265) - 90.0f;
        float pitch  = (float)(-(std::atan2(dy, dist) * 180.0 / 3.14159265));

        return { yaw, pitch };
    }

    // ===== Is player in GUI =====
    static bool IsInGui(JNIEnv* env) {
        jobject mc = GetInstance(env);
        if (!mc || !fCurrentScreen) return true; // assume in GUI if we can't tell
        jobject screen = env->GetObjectField(mc, fCurrentScreen);
        return screen != nullptr;
    }
};
