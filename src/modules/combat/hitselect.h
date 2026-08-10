#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../input/click_scheduler.h"
#include <Windows.h>
#include <cstdio>
#include <random>

// =================================================================
// Hit Select
// =================================================================
// Fires a click on the tick you take knockback. Landing your own
// hit on the same tick you receive one makes the server weigh the
// two against each other and shaves part of the push off. Strong
// players do this by feel; this does it by counting hurtTime.
//
// hurtTime counts DOWN from 10 after a hit, so:
//   10  the tick the damage arrived
//    9  one tick later
//    8  two ticks later, and so on
//
// The useful window is the first two or three ticks. Later than
// that and the knockback has already been applied and resolved.
//
// WHY IT GOES THROUGH THE SCHEDULER
// The autoclicker is usually running too. Two sources emitting
// independently can land microseconds apart, and a sub-20ms
// interval is the one thing no hand can produce. Every click in
// the client leaves through ClickScheduler, which holds a shared
// floor and drops anything that would break it.
// =================================================================

class HitSelect : public Module {
private:
    // hurtTime starts here and counts down, so "start" is the
    // earliest tick and the larger number.
    static constexpr int kHurtTimeOnHit = 10;

    // ---- Core ----
    float m_chance = 85.0f;

    // ---- Advanced: window ----
    int m_windowStart = 10;   // fire from this hurtTime
    int m_windowEnd   = 8;    // down to this one

    // ---- Advanced: gating ----
    bool  m_requireTarget = true;
    float m_targetRange   = 3.5f;
    bool  m_requireCombat = true;   // must be an actual exchange
    bool  m_respectRhythm = true;   // do not outpace your own clicking
    int   m_minGapMs      = 90;

    // ---- State ----
    int  m_lastHurtTime = 0;
    bool m_firedThisHit = false;

    // Readout
    int m_fired   = 0;
    int m_dropped = 0;
    int m_skipped = 0;
    const char* m_why = "idle";
    mutable char m_status[48] = "";
    mutable char m_notice[192] = "";

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

public:
    HitSelect() : Module("Hit Select", "Click on the tick you take knockback",
                         ModuleCategory::COMBAT, 0)
    {
        Bind("Chance", &m_chance, 10.0f, 100.0f, "%.0f%%",
             "How many incoming hits get answered");

        Bind("Window Start", &m_windowStart, 1, 10,
             "Earliest tick after the hit. 10 is the tick the damage "
             "arrived, which is the strongest and the most obvious.")
            .Advanced();

        Bind("Window End", &m_windowEnd, 1, 10,
             "Latest tick after the hit. Past three ticks the knockback "
             "has already resolved.")
            .Advanced();

        Bind("Require Target", &m_requireTarget,
             "Swinging at nothing on the exact tick you take damage is not "
             "something a player does")
            .Advanced();

        Bind("Target Range", &m_targetRange, 2.0f, 6.0f, "%.1f",
             "How close something has to be to count")
            .When("Require Target", 1).Advanced();

        Bind("Require Combat", &m_requireCombat,
             "Ignore fall damage and fire ticks")
            .Advanced();

        Bind("Respect Your Rhythm", &m_respectRhythm,
             "Never click sooner after your own swing than you already "
             "were. An interval that only appears after damage is its own "
             "fingerprint.")
            .Advanced();

        Bind("Min Gap", &m_minGapMs, 40, 200,
             "Milliseconds that must have passed since your last swing")
            .When("Respect Your Rhythm", 1).Advanced();
    }

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) { m_why = "menu"; return; }

        // A hand-edited config can invert the window; the sliders
        // cannot, and an inverted window silently never fires.
        if (m_windowEnd > m_windowStart) m_windowEnd = m_windowStart;

        int hurtTime = CombatState::HurtTime();

        // A fresh hit arrived: the window reopens
        if (hurtTime > m_lastHurtTime) m_firedThisHit = false;
        m_lastHurtTime = hurtTime;

        // Outside the damage window there is nothing to cancel
        if (hurtTime > m_windowStart || hurtTime < m_windowEnd) {
            m_why = hurtTime == 0 ? "not hurt" : "outside window";
            return;
        }

        // One click per hit. Spraying across the whole window is a
        // burst of clicks locked to incoming damage, which is a far
        // louder signal than the technique is worth.
        if (m_firedThisHit) { m_why = "already fired"; return; }

        if (m_requireCombat && !CombatState::InCombat()) {
            m_why = "not fighting";
            return;
        }

        if (m_requireTarget) {
            if (!EntityList::Init(env)) return;
            if (EntityList::GetPlayers(env, m_targetRange).empty()) {
                m_why = "nothing in reach";
                return;
            }
        }

        // Do not fire faster than you were already clicking.
        if (m_respectRhythm) {
            long long since = CombatState::MsSinceSwing();
            if (since < m_minGapMs) {
                m_skipped++;
                m_why = "too soon after your last swing";
                return;
            }
        }

        if (!Roll(m_chance)) { m_why = "missed one"; return; }

        if (ClickScheduler::RequestClick(false)) {
            m_fired++;
            m_firedThisHit = true;
            m_why = "fired";
        } else {
            m_dropped++;
            m_why = "blocked by click floor";
        }
    }

    void OnDisable(JNIEnv*) override {
        m_firedThisHit = false;
        m_lastHurtTime = 0;
        m_status[0] = '\0';
    }

    // hurtTime is zero in a fresh world, so a stale one would look
    // like a hit that never happened.
    void OnReset(JNIEnv*) override {
        m_firedThisHit = false;
        m_lastHurtTime = 0;
        m_fired = m_dropped = m_skipped = 0;
        m_why = "idle";
    }

    const char* StatusLine() const override {
        snprintf(m_status, sizeof(m_status), "%d fired", m_fired);
        return m_status;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!CombatState::IsUsable()) {
            *text = "Swing detection could not be resolved, so the rhythm "
                    "check is blind and the timing will be cruder.";
            return NoticeLevel::Warning;
        }
        if (m_dropped > m_fired && m_fired > 4) {
            *text = "Most of these are being dropped: the autoclicker is "
                    "already using the whole click budget. Lower its CPS if "
                    "you want this to land.";
            return NoticeLevel::Warning;
        }
        snprintf(m_notice, sizeof(m_notice),
                 "Fires between %d and %d ticks after a hit lands on you, "
                 "which cancels part of the knockback.",
                 kHurtTimeOnHit - m_windowStart, kHurtTimeOnHit - m_windowEnd);
        *text = m_notice;
        return NoticeLevel::Info;
    }
};
