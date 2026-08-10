#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include <imgui.h>
#include <cmath>
#include <random>

// =================================================================
// Speed
// =================================================================
// There are two completely different things hiding under this name
// and mixing them up is why most clients get people banned.
//
//   Sprint Jump is not a cheat. In 1.8 a sprinting player who jumps
//   gets a 1.3x forward boost on takeoff and keeps almost all of it
//   through the air, because air friction is far lower than ground
//   friction. Continuous hopping is measurably faster than running
//   flat. Every good 1.8 player already does it by hand. We drive
//   GameSettings.keyBindJump.pressed and nothing else, so the game
//   builds the movement itself and the packet stream is identical
//   to a human spamming space. There is nothing for an anticheat to
//   see, because nothing about the movement is abnormal.
//
//   Strafe and BHop overwrite the motion vector. A prediction
//   anticheat re-simulates your movement from your inputs and
//   compares the result to where you actually are. Any multiplier
//   shows up on the first tick. No amount of tuning fixes this.
//   They are here for unprotected servers and labelled as such.
//
// MODES
//   0 Sprint Jump   input only, undetectable, default
//   1 Strafe        rewrites the motion vector, detected
//   2 BHop          hop plus a motion multiplier, detected
// =================================================================

class Speed : public Module {
private:
    int   m_mode       = 0;
    float m_multiplier = 1.6f;
    bool  m_groundOnly = false;

    // ---- Sprint Jump ----
    bool  m_requireSprint  = true;   // only hop when actually sprinting
    bool  m_forwardOnly    = true;   // no hopping while strafing sideways
    bool  m_pauseInCombat  = true;   // leave the ground to More KB / Velocity
    float m_skipChance     = 7.0f;   // human hop streaks are not perfect
    int   m_groundTicksMin = 1;      // ticks on the ground between hops
    int   m_groundTicksMax = 2;

    bool m_holdingJump   = false;
    int  m_groundTicks   = 0;
    int  m_targetGround  = 1;
    int  m_hops          = 0;

    std::mt19937 m_rng{ std::random_device{}() };

    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        return std::uniform_int_distribution<int>(lo, hi)(m_rng);
    }
    bool Roll(float pct) {
        return std::uniform_real_distribution<float>(0.f, 100.f)(m_rng) < pct;
    }

    void ReleaseJump(JNIEnv* env) {
        if (!m_holdingJump) return;
        KeyBinds::ReleaseJump(env);
        m_holdingJump = false;
    }

    // -------------------------------------------------------------
    // Sprint Jump
    // -------------------------------------------------------------
    // The jump key has to go back up between hops. EntityPlayerSP
    // only jumps on the tick the key is held while on the ground,
    // and holding it down permanently produces a rigid hop every
    // landing tick, which is a pattern in itself. A short random
    // pause on the ground breaks that up and costs almost no speed.
    void TickSprintJump(JNIEnv* env, jobject player) {
        bool onGround = Minecraft::IsOnGround(env, player);

        if (!onGround) {                    // airborne: key must be up
            ReleaseJump(env);
            m_groundTicks = 0;
            return;
        }

        ReleaseJump(env);                   // land first, then decide
        m_groundTicks++;

        if (KeyBinds::GetSneak(env)) return;
        if (m_pauseInCombat && CombatState::HitTakenThisTick()) return;

        bool fwd   = KeyBinds::GetForward(env);
        bool left  = KeyBinds::GetLeft(env);
        bool right = KeyBinds::GetRight(env);
        bool moving = m_forwardOnly ? fwd : (fwd || left || right);
        if (!moving) { m_hops = 0; return; }

        // Actually travelling, not walking into a wall
        double mx = Minecraft::GetMotionX(env, player);
        double mz = Minecraft::GetMotionZ(env, player);
        if (std::sqrt(mx * mx + mz * mz) < 0.08) return;

        if (m_requireSprint && !Minecraft::IsSprinting(env, player)) return;

        if (m_groundTicks < m_targetGround) return;

        if (Roll(m_skipChance)) {           // miss one like a person does
            m_targetGround = Rand(m_groundTicksMin, m_groundTicksMax) + 2;
            m_groundTicks  = 0;
            return;
        }

        KeyBinds::SetJump(env, true);
        m_holdingJump  = true;
        m_groundTicks  = 0;
        m_targetGround = Rand(m_groundTicksMin, m_groundTicksMax);
        m_hops++;
    }

    // -------------------------------------------------------------
    // Motion modes
    // -------------------------------------------------------------
    static float MoveAngle(float yaw, float forward, float strafe) {
        if (forward > 0.f) {
            if (strafe > 0.f)      return yaw - 45.f;
            if (strafe < 0.f)      return yaw + 45.f;
            return yaw;
        }
        if (forward < 0.f) {
            if (strafe > 0.f)      return yaw - 135.f;
            if (strafe < 0.f)      return yaw + 135.f;
            return yaw + 180.f;
        }
        if (strafe > 0.f)          return yaw - 90.f;
        if (strafe < 0.f)          return yaw + 90.f;
        return yaw;
    }

    void TickMotion(JNIEnv* env, jobject player) {
        static constexpr double kBaseSpeed = 0.2873;   // vanilla sprint

        bool onGround = Minecraft::IsOnGround(env, player);
        if (m_groundOnly && !onGround) return;

        bool fwd   = KeyBinds::GetForward(env);
        bool back  = KeyBinds::GetBack(env);
        bool left  = KeyBinds::GetLeft(env);
        bool right = KeyBinds::GetRight(env);
        if (!fwd && !back && !left && !right) return;

        if (m_mode == 2) {                  // BHop
            if (onGround) Minecraft::SetMotionY(env, player, 0.4);
            double mx = Minecraft::GetMotionX(env, player);
            double mz = Minecraft::GetMotionZ(env, player);
            Minecraft::SetMotionX(env, player, mx * m_multiplier);
            Minecraft::SetMotionZ(env, player, mz * m_multiplier);
            return;
        }

        float forward = (fwd ? 1.f : 0.f) - (back ? 1.f : 0.f);
        float strafe  = (left ? 1.f : 0.f) - (right ? 1.f : 0.f);
        float angle   = MoveAngle(Minecraft::GetYaw(env, player), forward, strafe);

        double rad   = angle * 3.14159265358979 / 180.0;
        double speed = kBaseSpeed * m_multiplier;

        Minecraft::SetMotionX(env, player, -std::sin(rad) * speed);
        Minecraft::SetMotionZ(env, player,  std::cos(rad) * speed);
    }

public:
    Speed() : Module("Speed", "Sprint jump automatically, or force raw motion",
                     ModuleCategory::MOVEMENT, 'F')
    {
        Bind("Mode", &m_mode);
        Bind("Multiplier", &m_multiplier);
        Bind("Ground Only", &m_groundOnly);
        Bind("Require Sprint", &m_requireSprint);
        Bind("Forward Only", &m_forwardOnly);
        Bind("Pause In Combat", &m_pauseInCombat);
        Bind("Skip Chance", &m_skipChance);
        Bind("Ground Ticks Min", &m_groundTicksMin);
        Bind("Ground Ticks Max", &m_groundTicksMax);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { ReleaseJump(env); return; }

        if (Minecraft::IsInGui(env)) { ReleaseJump(env); return; }

        if (m_mode == 0) {
            if (!KeyBinds::HasJump()) return;
            TickSprintJump(env, player);
        } else {
            ReleaseJump(env);
            TickMotion(env, player);
        }
    }

    void OnDisable(JNIEnv* env) override {
        ReleaseJump(env);
        m_groundTicks = 0;
        m_hops = 0;
    }

    void RenderSettings() override {
        const char* modes[] = { "Sprint Jump", "Strafe", "BHop" };
        ImGui::Combo("Mode", &m_mode, modes, 3);

        if (m_mode == 0) {
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f),
                "Input only. Same packets as a player spamming space.");
            ImGui::Separator();
            ImGui::Checkbox("Require Sprint", &m_requireSprint);
            ImGui::Checkbox("Forward Only", &m_forwardOnly);
            ImGui::Checkbox("Pause After Taking A Hit", &m_pauseInCombat);
            if (m_pauseInCombat) {
                ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.85f, 1.f),
                    "Keeps the ground free for More KB and Velocity");
            }
            ImGui::Separator();
            ImGui::SliderFloat("Skip Chance", &m_skipChance, 0.f, 25.f, "%.0f%%");
            ImGui::SliderInt("Ground Ticks Min", &m_groundTicksMin, 1, 6);
            ImGui::SliderInt("Ground Ticks Max", &m_groundTicksMax, 1, 8);
            if (m_groundTicksMin > m_groundTicksMax)
                m_groundTicksMin = m_groundTicksMax;

            float avgGround = (m_groundTicksMin + m_groundTicksMax) * 0.5f;
            ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.85f, 1.f),
                "About one hop every %.1f ticks   (%d this session)",
                avgGround + 11.0f, m_hops);

            if (!KeyBinds::HasJump()) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                    "Jump keybind unresolved: module inactive");
            }
            return;
        }

        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f),
            "! Rewrites motion. Caught instantly by AGC, Grim and Polar");
        ImGui::Separator();
        ImGui::SliderFloat("Multiplier", &m_multiplier, 1.0f, 3.0f, "%.2fx");
        ImGui::Checkbox("Ground Only", &m_groundOnly);
    }
};
