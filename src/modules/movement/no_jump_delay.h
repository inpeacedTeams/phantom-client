#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>

// =================================================================
// No Jump Delay
// =================================================================
// Vanilla forces a 10-tick wait between jumps via
// EntityLivingBase.jumpTicks. Zeroing it every tick removes that.
//
// Worth knowing: jump frequency is one of the cheapest things for a
// prediction anticheat to check, because the vanilla minimum is a
// hard constant. This is safe on unprotected servers and nowhere
// else.
// =================================================================

class NoJumpDelay : public Module {
private:
    jfieldID m_fJumpTicks = nullptr;
    bool m_resolved = false;

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::entityLivingBase) {
            m_fJumpTicks = JvmtiUtil::FindField(env, ClassResolver::entityLivingBase,
                { "field_70773_bE", "jumpTicks" });
        }
        m_resolved = true;
    }

public:
    NoJumpDelay() : Module("No Jump Delay", "Remove the cooldown between jumps",
                           ModuleCategory::MOVEMENT, 0) {}

    void OnTick(JNIEnv* env) override {
        Resolve(env);
        if (!m_fJumpTicks) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        env->SetIntField(player, m_fJumpTicks, 0);
    }

    void RenderSettings() override {
        ImGui::TextWrapped("Removes the 10-tick delay between jumps.");
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
            "Jump frequency is trivially checked. Unprotected servers only.");
        if (!m_fJumpTicks) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "jumpTicks unresolved: module inactive");
        }
    }
};
