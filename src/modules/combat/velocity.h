#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>

class Velocity : public Module {
private:
    float m_horizontal = 85.0f; // % of original knockback
    float m_vertical = 100.0f;
    int m_mode = 0; // 0 = Packet, 1 = Cancel, 2 = Reverse
    bool m_onlyInCombat = true;

    int m_lastHurtTime = 0;

public:
    Velocity() : Module("Velocity", "Reduce knockback when hit", ModuleCategory::COMBAT, 'B') {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        int hurtTime = Minecraft::GetHurtTime(env, player);

        // Detect when we get hit (hurtTime goes from 0 to 10)
        if (hurtTime > 0 && m_lastHurtTime == 0) {
            // We just got hit, modify velocity
            double motionX = Minecraft::GetMotionX(env, player);
            double motionY = Minecraft::GetMotionY(env, player);
            double motionZ = Minecraft::GetMotionZ(env, player);

            switch (m_mode) {
                case 0: // Reduce
                    motionX *= (m_horizontal / 100.0);
                    motionZ *= (m_horizontal / 100.0);
                    motionY *= (m_vertical / 100.0);
                    break;
                case 1: // Cancel
                    motionX = 0;
                    motionY = 0;
                    motionZ = 0;
                    break;
                case 2: // Reverse
                    motionX *= -0.5;
                    motionZ *= -0.5;
                    break;
            }

            Minecraft::SetMotionX(env, player, motionX);
            Minecraft::SetMotionY(env, player, motionY);
            Minecraft::SetMotionZ(env, player, motionZ);
        }

        m_lastHurtTime = hurtTime;
    }

    void RenderSettings() override {
        const char* modes[] = { "Reduce", "Cancel", "Reverse" };
        ImGui::Combo("Mode", &m_mode, modes, 3);
        if (m_mode == 0) {
            ImGui::SliderFloat("Horizontal", &m_horizontal, 0.0f, 100.0f, "%.0f%%");
            ImGui::SliderFloat("Vertical", &m_vertical, 0.0f, 100.0f, "%.0f%%");
        }
        ImGui::Checkbox("Only In Combat", &m_onlyInCombat);
    }
};
