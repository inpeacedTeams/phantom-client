#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../mc/rotation.h"
#include <imgui.h>
#include <Windows.h>
#include <cmath>
#include <random>

// =================================================================
// Velocity — reduce INCOMING knockback
// =================================================================
// DIRECT MODES rewrite the motion vector after a hit. Instant and
// total, and the first thing any prediction anticheat checks: the
// server simulates your knockback and compares. Unprotected servers
// only.
//
// LEGIT MODES reproduce what strong players do by hand, through
// real keybinds, so the packets are indistinguishable from someone
// with good habits.
//
//   Jump Reset  A jump taken as the hit lands replaces your
//               vertical velocity with the jump arc and puts you
//               back on the ground sooner, so friction kills the
//               horizontal push earlier. Timing is everything: too
//               early and you eat the knockback airborne, which is
//               worse than doing nothing.
//
//   Strafe      Turning into the attacker rather than being pushed
//               straight back. The old version picked left or right
//               at random, which was as likely to throw you away
//               from them as toward them. It now works out which
//               side they are on and strafes that way.
//
// Both drive KeyBinds rather than motion fields, because writing
// moveStrafing directly does nothing: the movement code recomputes
// it from the keys every tick.
// =================================================================

class Velocity : public Module {
private:
    // 0 Reduce, 1 Cancel, 2 Reverse, 3 Jump, 4 Strafe, 5 Combined
    int m_mode = 5;

    // ---- Direct ----
    float m_horizontal = 85.0f;
    float m_vertical   = 100.0f;
    float m_directChance = 100.0f;

    // ---- Jump reset ----
    float m_jumpChance     = 80.0f;
    int   m_jumpDelayMin   = 0;
    int   m_jumpDelayMax   = 2;
    bool  m_jumpOnlyGround = true;
    int   m_hitsUntilJump  = 1;
    bool  m_jumpOnlyMoving = true;   // a standing jump does nothing useful

    // ---- Strafe ----
    float m_strafeChance = 70.0f;
    int   m_strafeDelay  = 1;
    int   m_strafeTicks  = 2;
    bool  m_strafeToward = true;     // into the attacker, not at random

    // ---- Gating ----
    bool m_onlyInCombat = false;

    // ---- State ----
    int  m_hitsTaken       = 0;
    bool m_pendingJump     = false;
    int  m_jumpCountdown   = 0;
    bool m_jumpHeld        = false;
    bool m_pendingStrafe   = false;
    int  m_strafeCountdown = 0;
    int  m_strafeHold      = 0;
    bool m_strafeLeft      = false;
    bool m_strafeActive    = false;

    // ---- Readout ----
    int m_jumps   = 0;
    int m_strafes = 0;
    const char* m_lastSide = "-";

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

    bool IsLegit() const    { return m_mode >= 3; }
    bool UsesJump() const   { return m_mode == 3 || m_mode == 5; }
    bool UsesStrafe() const { return m_mode == 4 || m_mode == 5; }

    void StopStrafe(JNIEnv* env) {
        if (!m_strafeActive) return;
        if (m_strafeLeft) KeyBinds::ReleaseLeft(env);
        else              KeyBinds::ReleaseRight(env);
        m_strafeActive = false;
    }

    void StopJump(JNIEnv* env) {
        if (!m_jumpHeld) return;
        KeyBinds::ReleaseJump(env);
        m_jumpHeld = false;
    }

    // Which side is the attacker on? Positive cross product means
    // they are to our right, so we strafe right to close on them.
    // Guessing at random, as the old build did, was a coin flip
    // between closing the gap and doubling the knockback.
    bool AttackerOnLeft(JNIEnv* env, jobject player) {
        if (!EntityList::Init(env)) return (m_rng() % 2) == 0;

        auto ents = EntityList::GetPlayers(env, 6.0f);
        if (ents.empty()) return (m_rng() % 2) == 0;

        const EntityInfo* near = nullptr;
        double best = 1e9;
        for (auto& e : ents) {
            if (e.distanceToPlayer < best) { best = e.distanceToPlayer; near = &e; }
        }
        if (!near) return (m_rng() % 2) == 0;

        double px = Minecraft::GetPosX(env, player);
        double pz = Minecraft::GetPosZ(env, player);
        float yaw = Minecraft::GetYaw(env, player);

        const double DEG = 3.14159265358979 / 180.0;
        double fx = -std::sin(yaw * DEG);
        double fz =  std::cos(yaw * DEG);

        double dx = near->posX - px;
        double dz = near->posZ - pz;

        // 2D cross product of facing against the direction to them
        double cross = fx * dz - fz * dx;
        return cross > 0.0;   // they are to our left
    }

public:
    Velocity() : Module("Velocity", "Reduce incoming knockback",
                        ModuleCategory::COMBAT, 'B')
    {
        Bind("Mode", &m_mode);
        Bind("Horizontal", &m_horizontal);
        Bind("Vertical", &m_vertical);
        Bind("Direct Chance", &m_directChance);
        Bind("Jump Chance", &m_jumpChance);
        Bind("Jump Delay Min", &m_jumpDelayMin);
        Bind("Jump Delay Max", &m_jumpDelayMax);
        Bind("Jump Only Ground", &m_jumpOnlyGround);
        Bind("Jump Only Moving", &m_jumpOnlyMoving);
        Bind("Hits Until Jump", &m_hitsUntilJump);
        Bind("Strafe Chance", &m_strafeChance);
        Bind("Strafe Delay", &m_strafeDelay);
        Bind("Strafe Ticks", &m_strafeTicks);
        Bind("Strafe Toward", &m_strafeToward);
        Bind("Only In Combat", &m_onlyInCombat);
    }

    void OnEnable(JNIEnv*) override {
        m_hitsTaken = 0;
        m_pendingJump = m_pendingStrafe = false;
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

        // Knockback from fall damage or fire is not worth reacting
        // to, and reacting to it is a tell.
        if (m_onlyInCombat && !CombatState::InCombat()) {
            StopStrafe(env);
            StopJump(env);
            return;
        }

        bool justHit  = CombatState::HitTakenThisTick();
        bool onGround = Minecraft::IsOnGround(env, player);

        // ---------------- Direct modes ----------------
        if (!IsLegit()) {
            if (!justHit) return;
            if (!Roll(m_directChance)) return;

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
                m_strafeLeft = m_strafeToward
                             ? AttackerOnLeft(env, player)
                             : ((m_rng() % 2) == 0);
                m_lastSide = m_strafeLeft ? "left" : "right";
            }
        }

        // ---- Jump ----
        // The key is released the tick after it went down: holding
        // it would queue a second jump the moment we land, which is
        // both slower and visibly bunny-hoppy.
        if (m_jumpHeld) StopJump(env);

        if (m_pendingJump) {
            if (m_jumpCountdown <= 0) {
                bool moving = KeyBinds::GetForward(env)
                           || KeyBinds::GetBack(env)
                           || KeyBinds::GetLeft(env)
                           || KeyBinds::GetRight(env);

                bool ok = (!m_jumpOnlyGround || onGround)
                       && (!m_jumpOnlyMoving || moving)
                       && onGround;   // a mid-air jump does nothing

                if (ok) {
                    // Pressing the key lets vanilla apply the jump,
                    // including the sprint boost. Writing motionY by
                    // hand skips that and reads as a teleport.
                    KeyBinds::SetJump(env, true);
                    m_jumpHeld = true;
                    m_jumps++;
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
                m_strafeActive  = true;
                m_strafeHold    = m_strafeTicks;
                m_pendingStrafe = false;
                m_strafes++;
            } else {
                m_strafeCountdown--;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        StopStrafe(env);
        StopJump(env);
        m_pendingJump = m_pendingStrafe = false;
        m_hitsTaken = 0;
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

        ImGui::TextDisabled("Jumps %d | strafes %d (last: %s)",
            m_jumps, m_strafes, m_lastSide);

        ImGui::Separator();

        if (m_mode <= 2) {
            if (m_mode == 0) {
                ImGui::SliderFloat("Horizontal", &m_horizontal, 0.f, 100.f, "%.0f%%");
                ImGui::SliderFloat("Vertical",   &m_vertical,   0.f, 100.f, "%.0f%%");
            }
            ImGui::SliderFloat("Chance", &m_directChance, 10.f, 100.f, "%.0f%%");
            ImGui::TextDisabled("Below 100%% some hits land normally, "
                                "which is harder to fingerprint.");
        }

        if (UsesJump()) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Jump Reset");
            ImGui::SliderFloat("Jump Chance", &m_jumpChance, 10.f, 100.f, "%.0f%%");
            ImGui::SliderInt("Delay Min", &m_jumpDelayMin, 0, 5);
            ImGui::SliderInt("Delay Max", &m_jumpDelayMax, 0, 5);
            if (m_jumpDelayMin > m_jumpDelayMax) m_jumpDelayMin = m_jumpDelayMax;
            ImGui::SliderInt("Hits Until Jump", &m_hitsUntilJump, 1, 5);
            ImGui::Checkbox("Only On Ground", &m_jumpOnlyGround);
            ImGui::Checkbox("Only While Moving", &m_jumpOnlyMoving);
            ImGui::Spacing();
        }

        if (UsesStrafe()) {
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f), "Strafe");
            ImGui::SliderFloat("Strafe Chance", &m_strafeChance, 10.f, 100.f, "%.0f%%");
            ImGui::SliderInt("Strafe Delay", &m_strafeDelay, 0, 5);
            ImGui::SliderInt("Strafe Ticks", &m_strafeTicks, 1, 6);
            ImGui::Checkbox("Toward Attacker", &m_strafeToward);
            ImGui::TextDisabled(m_strafeToward
                ? "Works out which side they are on and closes in."
                : "Picks a side at random, which often helps them.");
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Checkbox("Only In Combat", &m_onlyInCombat);
        ImGui::TextDisabled("Ignores fall damage and fire ticks.");

        if (IsLegit() && !KeyBinds::HasMovement()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Keybinds unresolved: legit modes inactive");
        }
    }
};
