#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include <imgui.h>

// =================================================================
// No Jump Delay
// =================================================================
// Removes the 10-tick (0.5s) cooldown between jumps.
// In vanilla, after jumping you must wait 10 ticks before
// jumping again. This module sets jumpTicks to 0 every tick.
//
// Useful for: speed bridging, combo PvP, general movement.
// Works by setting EntityLivingBase.jumpTicks = 0 via JNI.
// =================================================================

class NoJumpDelay : public Module {
private:
    jfieldID m_fJumpTicks = nullptr;
    bool m_jniResolved = false;

    void ResolveJNI(JNIEnv* env) {
        if (m_jniResolved) return;
        if (ClassResolver::entityLivingBase) {
            m_fJumpTicks = env->GetFieldID(ClassResolver::entityLivingBase, "field_70773_bE", "I");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                m_fJumpTicks = env->GetFieldID(ClassResolver::entityLivingBase, "jumpTicks", "I");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        m_jniResolved = true;
    }

public:
    NoJumpDelay() : Module("No Jump Delay", "Remove cooldown between jumps",
                           ModuleCategory::MOVEMENT, 0) {}

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);
        jobject player = Minecraft::GetPlayer(env);
        if (!player || !m_fJumpTicks) return;

        env->SetIntField(player, m_fJumpTicks, 0);
    }

    void RenderSettings() override {
        ImGui::TextWrapped("Removes the 10-tick delay between jumps.");
    }
};
