#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include <imgui.h>
#include <cmath>
#include <random>

// ==========================================================
// Velocity — Reduce INCOMING knockback
// ==========================================================
// Direct modes (weak ACs / non-Polar servers):
//   0 = Reduce  — scale motion by horizontal/vertical %
//   1 = Cancel  — zero out all motion on hit
//   2 = Reverse — reverse horizontal motion
//
// Legit modes (safe for Polar):
//   3 = Jump Reset — jump on hit to reset vertical KB
//   4 = Strafe     — redirect movement toward attacker
//   5 = Combined   — Jump Reset + Strafe
//
// Sprint Reset (More KB) is a SEPARATE module that increases
// OUTGOING knockback. See sprint_reset.h
// ==========================================================

class Velocity : public Module {
private:
    int m_mode = 5; // Combined (legit) by default

    // === Direct mode settings ===
    float m_horizontal = 85.0f;
    float m_vertical = 100.0f;

    // === Jump Reset settings ===
    float m_jumpChance = 80.0f;
    int m_jumpDelayMin = 0;
    int m_jumpDelayMax = 2;
    bool m_jumpOnlyGround = true;
    int m_hitsUntilJump = 1;

    // === Strafe settings ===
    float m_strafeStrength = 0.8f;
    int m_strafeDelay = 1;
    bool m_strafeOnlyFacing = true;

    // === Internal state ===
    int m_lastHurtTime = 0;
    int m_hitsTaken = 0;
    bool m_shouldJump = false;
    int m_jumpCountdown = 0;
    bool m_shouldStrafe = false;
    int m_strafeCountdown = 0;

    // JNI cached
    jfieldID m_fMoveStrafe = nullptr;
    bool m_jniResolved = false;

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

    int RandRange(int lo, int hi) {
        if (lo >= hi) return lo;
        std::uniform_int_distribution<int> d(lo, hi);
        return d(m_rng);
    }

    void ResolveJNI(JNIEnv* env) {
        if (m_jniResolved) return;
        if (ClassResolver::entityLivingBase) {
            m_fMoveStrafe = env->GetFieldID(ClassResolver::entityLivingBase, "field_70702_br", "F");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                m_fMoveStrafe = env->GetFieldID(ClassResolver::entityLivingBase, "moveStrafing", "F");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        m_jniResolved = true;
    }

    bool IsLegitMode() const { return m_mode >= 3; }
    bool UsesJumpReset() const { return m_mode == 3 || m_mode == 5; }
    bool UsesStrafe() const { return m_mode == 4 || m_mode == 5; }

public:
    Velocity() : Module("Velocity", "Reduce incoming knockback",
                        ModuleCategory::COMBAT, 'B') {}

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        int hurtTime = Minecraft::GetHurtTime(env, player);
        bool onGround = Minecraft::IsOnGround(env, player);
        bool justHit = (hurtTime > 0 && m_lastHurtTime == 0);

        // =============================================
        // DIRECT MODES (0-2)
        // =============================================
        if (!IsLegitMode() && justHit) {
            double mx = Minecraft::GetMotionX(env, player);
            double my = Minecraft::GetMotionY(env, player);
            double mz = Minecraft::GetMotionZ(env, player);

            switch (m_mode) {
                case 0: // Reduce
                    mx *= (m_horizontal / 100.0);
                    mz *= (m_horizontal / 100.0);
                    my *= (m_vertical / 100.0);
                    break;
                case 1: // Cancel
                    mx = my = mz = 0;
                    break;
                case 2: // Reverse
                    mx *= -0.5;
                    mz *= -0.5;
                    break;
            }

            Minecraft::SetMotionX(env, player, mx);
            Minecraft::SetMotionY(env, player, my);
            Minecraft::SetMotionZ(env, player, mz);
        }

        // =============================================
        // LEGIT MODES (3-5)
        // =============================================
        if (IsLegitMode() && justHit) {
            m_hitsTaken++;

            // Jump Reset trigger
            if (UsesJumpReset() && m_hitsTaken >= m_hitsUntilJump) {
                if (Roll(m_jumpChance)) {
                    m_jumpCountdown = RandRange(m_jumpDelayMin, m_jumpDelayMax);
                    m_shouldJump = true;
                }
                m_hitsTaken = 0;
            }

            // Strafe trigger
            if (UsesStrafe()) {
                m_strafeCountdown = m_strafeDelay;
                m_shouldStrafe = true;
            }
        }

        // --- Execute Jump Reset ---
        if (m_shouldJump) {
            if (m_jumpCountdown <= 0) {
                if ((!m_jumpOnlyGround || onGround) && onGround) {
                    Minecraft::SetMotionY(env, player, 0.42);
                }
                m_shouldJump = false;
            } else {
                m_jumpCountdown--;
            }
        }

        // --- Execute Strafe ---
        if (m_shouldStrafe) {
            if (m_strafeCountdown <= 0) {
                if (m_fMoveStrafe) {
                    float dir = (m_rng() % 2 == 0) ? 1.f : -1.f;
                    env->SetFloatField(player, m_fMoveStrafe, dir * m_strafeStrength);
                }
                m_shouldStrafe = false;
            } else {
                m_strafeCountdown--;
            }
        }

        m_lastHurtTime = hurtTime;
    }

    void RenderSettings() override {
        const char* modes[] = {
            "Reduce", "Cancel", "Reverse",      // 0-2: direct
            "Jump Reset", "Strafe",              // 3-4: legit single
            "Combined (Legit)"                    // 5:   legit all
        };
        ImGui::Combo("Mode", &m_mode, modes, 6);

        if (!IsLegitMode()) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "! Direct modes flag on Polar / strict ACs");
        }

        ImGui::Separator();

        // Direct
        if (m_mode == 0) {
            ImGui::SliderFloat("Horizontal", &m_horizontal, 0.f, 100.f, "%.0f%%");
            ImGui::SliderFloat("Vertical",   &m_vertical,   0.f, 100.f, "%.0f%%");
        }

        // Jump Reset
        if (UsesJumpReset()) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Jump Reset");
            ImGui::SliderFloat("Jump Chance",     &m_jumpChance, 10.f, 100.f, "%.0f%%");
            ImGui::SliderInt("Delay Min (ticks)",  &m_jumpDelayMin, 0, 5);
            ImGui::SliderInt("Delay Max (ticks)",  &m_jumpDelayMax, 0, 5);
            if (m_jumpDelayMin > m_jumpDelayMax) m_jumpDelayMin = m_jumpDelayMax;
            ImGui::SliderInt("Hits Until Jump",   &m_hitsUntilJump, 1, 5);
            ImGui::Checkbox("Only On Ground",     &m_jumpOnlyGround);
            ImGui::Spacing();
        }

        // Strafe
        if (UsesStrafe()) {
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f), "Strafe");
            ImGui::SliderFloat("Strength",     &m_strafeStrength, 0.1f, 1.f, "%.1f");
            ImGui::SliderInt("Strafe Delay",   &m_strafeDelay, 0, 5);
            ImGui::Checkbox("Only Facing",      &m_strafeOnlyFacing);
        }
    }
};
