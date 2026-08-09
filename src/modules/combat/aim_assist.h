#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>
#include <cmath>

class AimAssist : public Module {
private:
    float m_speed = 4.2f;      // Rotation speed (lower = smoother)
    float m_fov = 120.0f;       // Field of view cone (degrees)
    float m_maxRange = 6.0f;    // Max target range
    bool m_visibleOnly = true;  // Only target visible players
    int m_targetMode = 0;       // 0 = closest, 1 = lowest HP, 2 = crosshair

    float WrapAngle(float angle) {
        while (angle > 180.f)  angle -= 360.f;
        while (angle < -180.f) angle += 360.f;
        return angle;
    }

    float Lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

public:
    AimAssist() : Module("Aim Assist", "Smooth aim correction to nearest player", ModuleCategory::COMBAT, 'R') {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        jobject world = Minecraft::GetWorld(env);
        if (!player || !world) return;

        // Don't aim when in GUI
        if (Minecraft::IsInGui(env)) return;

        // Only aim when LMB is held (optional, more legit)
        // if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;

        // Get world.playerEntities (List<EntityPlayer>)
        // For now, we find the closest entity via JNI
        jobject bestTarget = nullptr;
        double bestDist = m_maxRange;

        // This is a simplified version - full impl would iterate
        // world.playerEntities list via JNI
        // For demonstration, we use the basic distance check:
        
        // Get player pos and rotation
        float playerYaw = Minecraft::GetYaw(env, player);
        float playerPitch = Minecraft::GetPitch(env, player);

        // TODO: iterate playerEntities list
        // For each entity:
        //   - Skip self
        //   - Check distance < m_maxRange  
        //   - Check FOV angle
        //   - Calculate rotation to target
        //   - Smooth rotate towards target

        if (bestTarget) {
            auto rot = Minecraft::GetRotationsTo(env, player, bestTarget);

            float yawDiff = WrapAngle(rot.yaw - playerYaw);
            float pitchDiff = rot.pitch - playerPitch;

            // Check FOV
            float angle = std::sqrt(yawDiff * yawDiff + pitchDiff * pitchDiff);
            if (angle > m_fov / 2.0f) return;

            // Smooth rotation
            float speed = m_speed * 0.01f; // normalize
            float newYaw = playerYaw + yawDiff * speed;
            float newPitch = playerPitch + pitchDiff * speed;

            // Clamp pitch
            if (newPitch > 90.f) newPitch = 90.f;
            if (newPitch < -90.f) newPitch = -90.f;

            Minecraft::SetYaw(env, player, newYaw);
            Minecraft::SetPitch(env, player, newPitch);
        }
    }

    void RenderSettings() override {
        ImGui::SliderFloat("Speed", &m_speed, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("FOV", &m_fov, 30.0f, 360.0f, "%.0f");
        ImGui::SliderFloat("Range", &m_maxRange, 1.0f, 8.0f, "%.1f");
        ImGui::Checkbox("Visible Only", &m_visibleOnly);

        const char* modes[] = { "Closest", "Lowest HP", "Crosshair" };
        ImGui::Combo("Target", &m_targetMode, modes, 3);
    }
};
