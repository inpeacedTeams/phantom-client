#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>
#include <chrono>
#include <random>

class KillAura : public Module {
private:
    float m_range = 3.8f;
    int m_minCPS = 12;
    int m_maxCPS = 16;
    bool m_autoBlock = true;
    bool m_playersOnly = true;
    bool m_multiTarget = false;
    int m_maxTargets = 3;

    std::chrono::steady_clock::time_point m_lastAttack;
    std::mt19937 m_rng{ std::random_device{}() };

    int GetRandomCPS() {
        std::uniform_int_distribution<int> dist(m_minCPS, m_maxCPS);
        return dist(m_rng);
    }

    long long GetAttackDelay() {
        int cps = GetRandomCPS();
        // Add jitter for realism
        std::uniform_int_distribution<int> jitter(-15, 15);
        return (1000 / cps) + jitter(m_rng);
    }

public:
    KillAura() : Module("Kill Aura", "Auto-attack entities in range", ModuleCategory::COMBAT, 'K') {
        m_lastAttack = std::chrono::steady_clock::now();
    }

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastAttack).count();

        if (elapsed < GetAttackDelay()) return;

        // TODO: Get entity list from world, find targets in range
        // For each valid target:
        //   1. Check distance <= m_range
        //   2. Check if player (if playersOnly)
        //   3. Check if alive (!isDead, hurtTime == 0 for anti-bot)
        //   4. Rotate to target (optional, can use with AimAssist)
        //   5. Send attack packet via playerController.attackEntity()
        //   6. Swing arm

        // Attack via JNI:
        // env->CallVoidMethod(playerController, attackEntityMethod, player, target);
        // env->CallVoidMethod(player, swingItemMethod);

        m_lastAttack = now;
    }

    void RenderSettings() override {
        ImGui::SliderFloat("Range", &m_range, 2.0f, 6.0f, "%.1f blocks");
        ImGui::SliderInt("Min CPS", &m_minCPS, 1, 20);
        ImGui::SliderInt("Max CPS", &m_maxCPS, 1, 20);
        if (m_minCPS > m_maxCPS) m_minCPS = m_maxCPS;
        ImGui::Checkbox("Auto Block", &m_autoBlock);
        ImGui::Checkbox("Players Only", &m_playersOnly);
        ImGui::Checkbox("Multi Target", &m_multiTarget);
        if (m_multiTarget) {
            ImGui::SliderInt("Max Targets", &m_maxTargets, 2, 8);
        }
    }
};
