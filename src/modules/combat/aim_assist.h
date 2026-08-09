#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include <imgui.h>
#include <Windows.h>
#include <cmath>
#include <random>
#include <algorithm>

// =================================================================
// Aim Assist
// =================================================================
// Nudges your crosshair toward a target instead of snapping to it.
// Speed scales down as the crosshair closes in, which removes the
// tell-tale overshoot-and-correct wobble a naive lerp produces.
//
// Randomization and aim breaks exist to keep the rotation delta
// distribution away from anything a GCD or entropy check can lock
// onto. Turning them off makes the module far easier to detect.
// =================================================================

class AimAssist : public Module {
private:
    float m_speed             = 4.2f;
    float m_pitchSpeed        = 3.0f;
    float m_fov               = 120.0f;
    float m_maxRange          = 4.5f;
    int   m_targetMode        = 2;      // 0=closest 1=lowest HP 2=crosshair
    bool  m_onlyWhileAttacking = false;
    bool  m_smoothYaw         = true;
    bool  m_smoothPitch       = true;
    float m_randomization     = 15.0f;
    bool  m_breakAim          = true;
    float m_breakChance       = 5.0f;
    float m_aimHeight         = 1.0f;   // Where on the body to aim

    std::mt19937 m_rng{ std::random_device{}() };

    static float WrapAngle(float a) {
        while (a > 180.f)  a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    }

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

public:
    AimAssist() : Module("Aim Assist", "Smooth aim correction toward the nearest player",
                         ModuleCategory::COMBAT, 'R') {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        if (m_onlyWhileAttacking && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;

        // Skipping the occasional tick produces the small misses a
        // human makes, instead of perfect frame-by-frame tracking.
        if (m_breakAim && Roll(m_breakChance)) return;

        if (!EntityList::Init(env)) return;

        auto entities = EntityList::GetPlayers(env, m_maxRange);
        if (entities.empty()) return;

        EntityInfo* target = nullptr;
        switch (m_targetMode) {
            case 0:  target = EntityList::FindClosest(entities, m_maxRange); break;
            case 1:  target = EntityList::FindLowestHP(entities, m_maxRange); break;
            default: target = EntityList::FindClosestToCrosshair(env, entities,
                                    m_fov, m_maxRange); break;
        }
        if (!target) return;

        float curYaw   = Minecraft::GetYaw(env, player);
        float curPitch = Minecraft::GetPitch(env, player);

        auto rot = Minecraft::GetRotationsToPos(env, player,
            target->posX, target->posY + m_aimHeight, target->posZ);

        float yawDiff   = WrapAngle(rot.yaw - curYaw);
        float pitchDiff = rot.pitch - curPitch;

        float angle = std::sqrt(yawDiff * yawDiff + pitchDiff * pitchDiff);
        if (angle > m_fov * 0.5f) return;

        float yawStep   = m_speed      * 0.015f;
        float pitchStep = m_pitchSpeed * 0.015f;

        if (m_randomization > 0.f) {
            float r = m_randomization * 0.01f;
            std::uniform_real_distribution<float> d(-r, r);
            yawStep   *= (1.0f + d(m_rng));
            pitchStep *= (1.0f + d(m_rng));
        }

        // Ease off as the crosshair closes in, so the aim settles
        // instead of oscillating around the target.
        float yawEase   = std::min(1.0f, std::fabs(yawDiff)   / 30.0f);
        float pitchEase = std::min(1.0f, std::fabs(pitchDiff) / 20.0f);

        float newYaw   = curYaw;
        float newPitch = curPitch;
        if (m_smoothYaw)   newYaw   = curYaw   + yawDiff   * yawStep   * yawEase;
        if (m_smoothPitch) newPitch = curPitch + pitchDiff * pitchStep * pitchEase;

        newPitch = std::max(-90.0f, std::min(90.0f, newPitch));

        Minecraft::SetYaw(env, player, newYaw);
        Minecraft::SetPitch(env, player, newPitch);
    }

    void RenderSettings() override {
        ImGui::SliderFloat("Yaw Speed", &m_speed, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("Pitch Speed", &m_pitchSpeed, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("FOV", &m_fov, 20.0f, 360.0f, "%.0f");
        ImGui::SliderFloat("Range", &m_maxRange, 1.0f, 6.0f, "%.1f");
        ImGui::SliderFloat("Aim Height", &m_aimHeight, 0.0f, 1.8f, "%.2f");

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
        if (m_breakAim)
            ImGui::SliderFloat("Break Chance", &m_breakChance, 1.f, 25.f, "%.0f%%");

        if (m_randomization < 5.f || !m_breakAim) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Low randomization is easy to fingerprint");
        }
    }
};
