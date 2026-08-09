#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include <imgui.h>
#include <cmath>
#include <random>

// =================================================================
// Aim Assist — Smooth rotation toward target player
// =================================================================
// Uses EntityList to find actual players in the world.
// Target selection: closest to crosshair, closest distance, or lowest HP.
// Smooth rotation with randomized speed for legit appearance.
// =================================================================

class AimAssist : public Module {
private:
    float m_speed = 4.2f;        // Rotation speed (higher = faster snap)
    float m_fov = 120.0f;        // FOV cone in degrees
    float m_maxRange = 4.5f;     // Max targeting range
    bool m_visibleOnly = true;   // Only visible players (TODO: raycast)
    int m_targetMode = 2;        // 0=closest dist, 1=lowest HP, 2=crosshair
    bool m_onlyWhileAttacking = false; // Only when LMB held
    bool m_smoothYaw = true;
    bool m_smoothPitch = true;
    float m_pitchSpeed = 3.0f;   // Separate pitch speed (usually slower)
    float m_randomization = 15.0f; // % random jitter for legit
    bool m_breakAim = true;      // Occasional aim breaks for legit
    float m_breakChance = 5.0f;

    std::mt19937 m_rng{ std::random_device{}() };

    float WrapAngle(float angle) {
        while (angle > 180.f)  angle -= 360.f;
        while (angle < -180.f) angle += 360.f;
        return angle;
    }

public:
    AimAssist() : Module("Aim Assist", "Smooth aim correction to nearest player",
                         ModuleCategory::COMBAT, 'R') {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        // Only when attacking (optional)
        if (m_onlyWhileAttacking && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            return;

        // Aim break: occasionally skip a tick
        if (m_breakAim) {
            std::uniform_real_distribution<float> d(0.f, 100.f);
            if (d(m_rng) < m_breakChance) return;
        }

        // Init entity list
        EntityList::Init(env);

        // Get all players in range
        auto entities = EntityList::GetPlayers(env, m_maxRange);
        if (entities.empty()) return;

        // Find target based on mode
        EntityInfo* target = nullptr;

        switch (m_targetMode) {
            case 0: // Closest distance
                target = EntityList::FindClosest(entities, m_maxRange);
                break;
            case 1: // Lowest HP
                target = EntityList::FindLowestHP(entities, m_maxRange);
                break;
            case 2: // Closest to crosshair
                target = EntityList::FindClosestToCrosshair(env, entities, m_fov, m_maxRange);
                break;
        }

        if (!target) return;

        // Current rotation
        float playerYaw = Minecraft::GetYaw(env, player);
        float playerPitch = Minecraft::GetPitch(env, player);

        // Target rotation (aim at target's eye height)
        auto rot = Minecraft::GetRotationsTo(env, player, target->ref);

        float yawDiff = WrapAngle(rot.yaw - playerYaw);
        float pitchDiff = rot.pitch - playerPitch;

        // Check if target is within FOV
        float totalAngle = std::sqrt(yawDiff * yawDiff + pitchDiff * pitchDiff);
        if (totalAngle > m_fov / 2.0f) return;

        // Smooth rotation with speed
        float yawSpeed = m_speed * 0.015f;   // Normalize to reasonable range
        float pitSpeed = m_pitchSpeed * 0.015f;

        // Add randomization for legit appearance
        if (m_randomization > 0) {
            float randRange = m_randomization * 0.01f;
            std::uniform_real_distribution<float> rd(-randRange, randRange);
            yawSpeed *= (1.0f + rd(m_rng));
            pitSpeed *= (1.0f + rd(m_rng));
        }

        // Distance-based speed: faster when far from target angle,
        // slower when close (prevents jitter)
        float yawFactor = std::min(1.0f, std::abs(yawDiff) / 30.0f);
        float pitchFactor = std::min(1.0f, std::abs(pitchDiff) / 20.0f);

        float newYaw = playerYaw;
        float newPitch = playerPitch;

        if (m_smoothYaw) {
            newYaw = playerYaw + yawDiff * yawSpeed * yawFactor;
        }
        if (m_smoothPitch) {
            newPitch = playerPitch + pitchDiff * pitSpeed * pitchFactor;
        }

        // Clamp pitch
        newPitch = std::max(-90.f, std::min(90.f, newPitch));

        // Apply
        Minecraft::SetYaw(env, player, newYaw);
        Minecraft::SetPitch(env, player, newPitch);
    }

    void RenderSettings() override {
        ImGui::SliderFloat("Yaw Speed", &m_speed, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("Pitch Speed", &m_pitchSpeed, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("FOV", &m_fov, 30.0f, 360.0f, "%.0f");
        ImGui::SliderFloat("Range", &m_maxRange, 1.0f, 6.0f, "%.1f");

        const char* modes[] = { "Closest", "Lowest HP", "Crosshair" };
        ImGui::Combo("Target", &m_targetMode, modes, 3);

        ImGui::Separator();

        ImGui::Checkbox("Only While Clicking", &m_onlyWhileAttacking);
        ImGui::Checkbox("Smooth Yaw", &m_smoothYaw);
        ImGui::Checkbox("Smooth Pitch", &m_smoothPitch);

        ImGui::Separator();
        ImGui::TextDisabled("Anti-detection");
        ImGui::SliderFloat("Randomization", &m_randomization, 0.f, 40.f, "%.0f%%");
        ImGui::Checkbox("Aim Breaks", &m_breakAim);
        if (m_breakAim) {
            ImGui::SliderFloat("Break Chance", &m_breakChance, 1.f, 20.f, "%.0f%%");
        }
    }
};
