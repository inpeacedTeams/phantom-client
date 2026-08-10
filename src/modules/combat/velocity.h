#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/movement.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include <Windows.h>
#include <cmath>
#include <cstdio>
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
//
// NOTE: do not name anything in here 'near' or 'far'. windef.h
// still defines both as empty macros for 16-bit compatibility, so
// a local called 'near' silently expands to nothing and the parse
// falls apart several lines later.
// =================================================================

class Velocity : public Module {
private:
    enum Mode { REDUCE = 0, CANCEL, REVERSE, JUMP, STRAFE, COMBINED };

    static constexpr const char* kModes[] = {
        "Reduce", "Cancel", "Reverse", "Jump", "Strafe", "Both"
    };

    int m_mode = COMBINED;

    // ---- Direct ----
    float m_horizontal = 85.0f;
    float m_vertical   = 100.0f;
    float m_directChance = 100.0f;

    // ---- Jump reset ----
    float m_jumpChance     = 80.0f;
    int   m_jumpDelayMin   = 0;
    int   m_jumpDelayMax   = 2;
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
    mutable char m_status[64] = "";

    // How far away something still counts as the thing that hit you.
    // Reach plus a margin; anything further did not land this hit.
    static constexpr float kAttackerSearch = 6.0f;

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
    bool CoinFlip() { return (m_rng() % 2) == 0; }

    bool IsLegit() const    { return m_mode >= JUMP; }
    bool UsesJump() const   { return m_mode == JUMP || m_mode == COMBINED; }
    bool UsesStrafe() const { return m_mode == STRAFE || m_mode == COMBINED; }

    void StopStrafe(JNIEnv* env) {
        if (!m_strafeActive) return;
        if (env) {
            if (m_strafeLeft) KeyBinds::ReleaseLeft(env);
            else              KeyBinds::ReleaseRight(env);
        }
        m_strafeActive = false;
    }

    void StopJump(JNIEnv* env) {
        if (!m_jumpHeld) return;
        if (env) KeyBinds::ReleaseJump(env);
        m_jumpHeld = false;
    }

    // Which side is the attacker on? A positive 2D cross product of
    // our facing against the direction to them means they are to
    // our left, so we strafe left to close in.
    //
    // Guessing at random, as the old build did, was a coin flip
    // between closing the gap and doubling the knockback.
    bool AttackerOnLeft(JNIEnv* env, jobject player) {
        if (!EntityList::Init(env)) return CoinFlip();

        auto ents = EntityList::GetPlayers(env, kAttackerSearch);
        if (ents.empty()) return CoinFlip();

        const EntityInfo* closest = nullptr;
        double best = 1e9;
        for (auto& e : ents) {
            if (e.distanceToPlayer < best) {
                best = e.distanceToPlayer;
                closest = &e;
            }
        }
        if (!closest) return CoinFlip();

        double px = Minecraft::GetPosX(env, player);
        double pz = Minecraft::GetPosZ(env, player);
        float yaw = Minecraft::GetYaw(env, player);

        double fx = -std::sin(yaw * Movement::kDegToRad);
        double fz =  std::cos(yaw * Movement::kDegToRad);

        double dx = closest->posX - px;
        double dz = closest->posZ - pz;

        double cross = fx * dz - fz * dx;
        return cross > 0.0;   // they are to our left
    }

public:
    Velocity() : Module("Velocity", "Reduce incoming knockback",
                        ModuleCategory::COMBAT, 'B')
    {
        BindMode("Mode", &m_mode, kModes, 6,
                 "Jump, Strafe and Both press real keys, so the server sees a "
                 "player with good habits. The first three rewrite your "
                 "motion and every prediction anticheat checks exactly that.");

        Bind("Strength", &m_horizontal, 0.0f, 100.0f, "%.0f%%",
             "How much of the push you keep")
            .When("Mode", REDUCE);

        Bind("Chance", &m_directChance, 10.0f, 100.0f, "%.0f%%",
             "How often it fires at all")
            .WhenAny("Mode", REDUCE, CANCEL, REVERSE);

        Bind("Jump Chance", &m_jumpChance, 10.0f, 100.0f, "%.0f%%",
             "How often a hit is answered with a jump")
            .WhenAny("Mode", JUMP, COMBINED);

        Bind("Strafe Chance", &m_strafeChance, 10.0f, 100.0f, "%.0f%%",
             "How often a hit is answered with a strafe")
            .WhenAny("Mode", STRAFE, COMBINED);

        // ---- Advanced ----
        Bind("Vertical", &m_vertical, 0.0f, 100.0f, "%.0f%%",
             "How much of the upward push you keep")
            .When("Mode", REDUCE).Advanced();

        Bind("Jump Delay Min", &m_jumpDelayMin, 0, 5,
             "Ticks to wait before jumping. Zero is the strongest reset and "
             "also the most machine-like.")
            .WhenAny("Mode", JUMP, COMBINED).Advanced();

        Bind("Jump Delay Max", &m_jumpDelayMax, 0, 5)
            .WhenAny("Mode", JUMP, COMBINED).Advanced();

        Bind("Hits Until Jump", &m_hitsUntilJump, 1, 5,
             "Answer every Nth hit rather than all of them")
            .WhenAny("Mode", JUMP, COMBINED).Advanced();

        Bind("Only While Moving", &m_jumpOnlyMoving,
             "A standing jump gains nothing and looks odd")
            .WhenAny("Mode", JUMP, COMBINED).Advanced();

        Bind("Strafe Delay", &m_strafeDelay, 0, 5,
             "Ticks between the hit and the strafe")
            .WhenAny("Mode", STRAFE, COMBINED).Advanced();

        Bind("Strafe Ticks", &m_strafeTicks, 1, 6,
             "How long the key is held")
            .WhenAny("Mode", STRAFE, COMBINED).Advanced();

        Bind("Toward Attacker", &m_strafeToward,
             "Works out which side they are on. Off picks at random, which "
             "is often the wrong way.")
            .WhenAny("Mode", STRAFE, COMBINED).Advanced();

        Bind("Only In Combat", &m_onlyInCombat,
             "Ignore knockback from fall damage and fire")
            .Advanced();
    }

    void OnEnable(JNIEnv*) override {
        m_hitsTaken = 0;
        m_pendingJump = m_pendingStrafe = false;
    }

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) { StopStrafe(env); StopJump(env); return; }

        if (IsLegit() && !KeyBinds::Init(env)) return;

        // A hand-edited config can invert these; a slider cannot.
        if (m_jumpDelayMin > m_jumpDelayMax) m_jumpDelayMin = m_jumpDelayMax;

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
                case REDUCE:
                    mx *= (m_horizontal / 100.0);
                    mz *= (m_horizontal / 100.0);
                    my *= (m_vertical   / 100.0);
                    break;
                case CANCEL:  mx = my = mz = 0.0; break;
                case REVERSE: mx *= -0.5; mz *= -0.5; break;
                default: break;
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
                             : CoinFlip();
            }
        }

        // ---- Jump ----
        // The key is released the tick after it went down: holding
        // it would queue a second jump the moment we land, which is
        // both slower and visibly bunny-hoppy.
        if (m_jumpHeld) StopJump(env);

        if (m_pendingJump) {
            if (m_jumpCountdown <= 0) {
                bool moving = Movement::Read(env).moving;

                bool ok = (!m_jumpOnlyMoving || moving)
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
        m_status[0] = '\0';
    }

    // A respawn must not leave a strafe key held down, and the hit
    // counter belongs to a fight that is over.
    void OnReset(JNIEnv* env) override {
        StopStrafe(env);
        StopJump(env);
        m_pendingJump = m_pendingStrafe = false;
        m_hitsTaken = 0;
        m_jumps = m_strafes = 0;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!IsLegit()) {
            *text = "Rewrites your motion after a hit, which is the first "
                    "thing Polar, AGC and Grim check. Unprotected servers "
                    "only.";
            return NoticeLevel::Danger;
        }
        if (!KeyBinds::HasMovement()) {
            *text = "The movement keybinds could not be found in this build "
                    "of the game, so the module cannot do anything.";
            return NoticeLevel::Warning;
        }
        return NoticeLevel::None;
    }

    const char* StatusLine() const override {
        if (!IsLegit()) {
            snprintf(m_status, sizeof(m_status), "%s", kModes[m_mode]);
        } else if (m_mode == JUMP) {
            snprintf(m_status, sizeof(m_status), "%d resets", m_jumps);
        } else if (m_mode == STRAFE) {
            snprintf(m_status, sizeof(m_status), "%d strafes", m_strafes);
        } else {
            snprintf(m_status, sizeof(m_status), "%d resets, %d strafes",
                     m_jumps, m_strafes);
        }
        return m_status;
    }
};
