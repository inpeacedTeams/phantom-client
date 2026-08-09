#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include <imgui.h>
#include <Windows.h>
#include <cmath>
#include <random>

// =================================================================
// Velocity — reduce INCOMING knockback
// =================================================================
// Direct modes rewrite the motion vector. Fast, effective, and the
// first thing any modern anticheat checks. Use only where nothing
// is watching.
//
// Legit modes reproduce what strong players do by hand:
//   Jump Reset  jumping as the hit lands resets vertical velocity
//               and puts you back on the ground sooner
//   Strafe      turning into the attacker instead of being pushed
//               straight back
//
// Both drive real KeyBindings. Writing moveStrafing directly does
// nothing, because the movement code recomputes it from the keys
// every tick.
// =================================================================

class Velocity : public Module {
private:
    int   m_mode = 5;   // 0 Reduce 1 Cancel 2 Reverse 3 Jump 4 Strafe 5 Combined

    float m_horizontal = 85.0f;
    float m_vertical   = 100.0f;

    float m_jumpChance     = 80.0f;
    int   m_jumpDelayMin   = 0;
    int   m_jumpDelayMax   = 2;
    bool  m_jumpOnlyGround = true;
    int   m_hitsUntilJump  = 1;

    float m_strafeChance   = 70.0f;
    int   m_strafeDelay    = 1;
    int   m_strafeTicks    = 2;

    // State
    int  m_lastHurtTime   = 0;
    int  m_hitsTaken      = 0;
    bool m_pendingJump    = false;
    int  m_jumpCountdown  = 0;
    bool m_jumpHeld       = false;
    bool m_pendingStrafe  = false;
    int  m_strafeCountdown = 0;
    int  m_strafeHold     = 0;
    bool m_strafeLeft     = false;
    bool m_strafeActive   = false;

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }
    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        std::uniform_int_distribution<int> d(lo, hi);
        return d(m_rng);
    }

    bool IsLegit() const      { return m_mode >= 3; }
    bool UsesJump() const     { return m_mode == 3 || m_mode == 5; }
    bool UsesStrafe() const   { return m_mode == 4 || m_mode == 5; }

    void StopStrafe(JNIEnv* env) {
        if (!m_strafeActive) return;
        if (m_strafeLeft) KeyBinds::SetLeft(env, false);
        else              KeyBinds::SetRight(env, false);
        m_strafeActive = false;
    }

    void StopJump(JNIEnv* env) {
        if (!m_jumpHeld) return;
        KeyBinds::SetJump(env, false);
        m_jumpHeld = false;
    }

public:
    Velocity() : Module("Velocity", "Reduce incoming knockback",
                        ModuleCategory::COMBAT, 'B')
    {
        Bind("Mode", &m_mode);
        Bind("Horizontal", &m_horizontal);
        Bind("Vertical", &m_vertical);
        Bind("Jump Chance", &m_jumpChance);
        Bind("Jump Delay Min", &m_jumpDelayMin);
        Bind("Jump Delay Max", &m_jumpDelayMax);
        Bind("Jump Only Ground", &m_jumpOnlyGround);
        Bind("Hits Until Jump", &m_hitsUntilJump);
        Bind("Strafe Chance", &m_strafeChance);
        Bind("Strafe Delay", &m_strafeDelay);
        Bind("Strafe Ticks", &m_strafeTicks);
    }

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (IsLegit() && !KeyBinds::Init(env)) return;

        if (Minecraft::IsInGui(env)) {
            StopStrafe(env);
            StopJump(env);
            return;
        }

        int  hurtTime = Minecraft::GetHurtTime(env, player);
        bool onGround = Minecraft::IsOnGround(env, player);
        bool justHit  = (hurtTime > 0 && m_lastHurtTime == 0);
        m_lastHurtTime = hurtTime;

        // ---------------- Direct modes ----------------
        if (!IsLegit()) {
            if (!justHit) return;
            double mx = Minecraft::GetMotionX(env, player);
            double my = Minecraft::GetMotionY(env, player);
            double mz = Minecraft::GetMotionZ(env, player);

            switch (m_mode) {
                case 0:
                    mx *= (m_horizontal / 100.0);
                    mz *= (m_horizontal / 100.0);
                    my *= (m_vertical   / 100.0);
                    break;
                case 1: mx = my = mz = 0.0; break;
                case 2: mx *= -0.5; mz *= -0.5; break;
            }

            Minecraft::SetMotionX(env, player, mx);
            Minecraft::SetMotionY(env, player, my);
            Minecraft::SetMotionZ(env, player, mz);
            return;
        }

        // ---------------- Legit modes ----------------
        if (justHit) {
            m_hitsTaken++;

            if (UsesJump() && m_hitsTaken >= m_hitsUntilJump) {
                if (Roll(m_jumpChance)) {
                    m_jumpCountdown = Rand(m_jumpDelayMin, m_jumpDelayMax);
                    m_pendingJump = true;
                }
                m_hitsTaken = 0;
            }

            if (UsesStrafe() && Roll(m_strafeChance)) {
                m_strafeCountdown = m_strafeDelay;
                m_pendingStrafe = true;
                m_strafeLeft = (m_rng() % 2) == 0;
            }
        }

        // ---- Jump: release the key we pressed last tick ----
        if (m_jumpHeld) StopJump(env);

        if (m_pendingJump) {
            if (m_jumpCountdown <= 0) {
                if (!m_jumpOnlyGround || onGround) {
                    // Pressing the key lets vanilla apply the jump,
                    // including the sprint boost. Writing motionY by
                    // hand skips that and reads as teleport-ish.
                    KeyBinds::SetJump(env, true);
                    m_jumpHeld = true;
                }
                m_pendingJump = false;
            } else {
                m_jumpCountdown--;
            }
        }

        // ---- Strafe ----
        if (m_strafeActive) {
            if (m_strafeHold <= 0) StopStrafe(env);
            else m_strafeHold--;
        }

        if (m_pendingStrafe && !m_strafeActive) {
            if (m_strafeCountdown <= 0) {
                if (m_strafeLeft) KeyBinds::SetLeft(env, true);
                else              KeyBinds::SetRight(env, true);
                m_strafeActive = true;
                m_strafeHold = m_strafeTicks;
                m_pendingStrafe = false;
            } else {
                m_strafeCountdown--;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        StopStrafe(env);
        StopJump(env);
        m_pendingJump = false;
        m_pendingStrafe = false;
    }

    void RenderSettings() override {
        const char* modes[] = {
            "Reduce", "Cancel", "Reverse",
            "Jump Reset", "Strafe", "Combined (Legit)"
        };
        ImGui::Combo("Mode", &m_mode, modes, 6);

        if (!IsLegit()) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "! Direct modes flag on Polar, AGC and Grim");
        }

        ImGui::Separator();

        if (m_mode == 0) {
            ImGui::SliderFloat("Horizontal", &m_horizontal, 0.f, 100.f, "%.0f%%");
            ImGui::SliderFloat("Vertical",   &m_vertical,   0.f, 100.f, "%.0f%%");
        }

        if (UsesJump()) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Jump Reset");
            ImGui::SliderFloat("Jump Chance", &m_jumpChance, 10.f, 100.f, "%.0f%%");
            ImGui::SliderInt("Delay Min", &m_jumpDelayMin, 0, 5);
            ImGui::SliderInt("Delay Max", &m_jumpDelayMax, 0, 5);
            if (m_jumpDelayMin > m_jumpDelayMax) m_jumpDelayMin = m_jumpDelayMax;
            ImGui::SliderInt("Hits Until Jump", &m_hitsUntilJump, 1, 5);
            ImGui::Checkbox("Only On Ground", &m_jumpOnlyGround);
            ImGui::Spacing();
        }

        if (UsesStrafe()) {
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f), "Strafe");
            ImGui::SliderFloat("Strafe Chance", &m_strafeChance, 10.f, 100.f, "%.0f%%");
            ImGui::SliderInt("Strafe Delay", &m_strafeDelay, 0, 5);
            ImGui::SliderInt("Strafe Ticks", &m_strafeTicks, 1, 6);
        }

        if (IsLegit() && !KeyBinds::HasMovement()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Keybinds unresolved: legit modes inactive");
        }
    }
};
