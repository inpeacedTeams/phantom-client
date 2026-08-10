#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/movement.h"
#include <cmath>
#include <cstdio>

// =================================================================
// Fly
// =================================================================
// Writes the motion vector every tick. There is no clever version
// of this: a prediction anticheat re-simulates your movement from
// your inputs, and "airborne and not falling" fails on the first
// tick. Unprotected servers only, and the panel says so.
//
// The direction maths is shared with Speed rather than being a
// second hand-written copy that had drifted out of step with it.
//
// MODES
//   0 Vanilla  full directional flight
//   1 Glide    cancel the fall, nothing else. Far quieter, because
//              slow descent is something a server sees from elytra
//              and slow-falling anyway.
// =================================================================

class Fly : public Module {
private:
    static constexpr const char* kModes[] = { "Vanilla", "Glide" };

    int   m_mode      = 1;      // Glide is the sane default
    float m_speed     = 2.0f;
    float m_glideRate = 0.02f;  // blocks per tick of descent

    // ---- Advanced ----
    bool  m_antiKick   = true;
    int   m_kickEvery  = 40;
    float m_kickDrop   = 0.04f;
    bool  m_stopOnLand = true;

    // Vertical speed is a fraction of the horizontal figure, so one
    // slider controls both and they stay in proportion.
    static constexpr double kVerticalRatio = 0.15;

    int  m_tick = 0;
    mutable char m_status[32] = {};

    void Stop(JNIEnv* env, jobject player) {
        if (!player) return;
        // Zeroing only Y used to leave you coasting sideways for a
        // second after the module went off.
        Minecraft::SetMotionX(env, player, 0.0);
        Minecraft::SetMotionY(env, player, 0.0);
        Minecraft::SetMotionZ(env, player, 0.0);
    }

public:
    Fly() : Module("Fly", "Stay in the air. Detected by prediction anticheats",
                   ModuleCategory::MOVEMENT, 0)
    {
        BindMode("Mode", &m_mode, kModes, 2,
                 "Glide only cancels your fall, which is far quieter than "
                 "full flight. Neither is safe on a prediction anticheat.");

        Bind("Speed", &m_speed, 0.5f, 5.0f, "%.1f",
             "Blocks per second, horizontal and vertical")
            .When("Mode", 0);

        Bind("Fall Rate", &m_glideRate, 0.0f, 0.2f, "%.3f",
             "Blocks per tick of descent. 0 is a hover.")
            .When("Mode", 1);

        Bind("Anti Kick", &m_antiKick,
             "A short dip now and then, so the server does not see a "
             "constant altitude")
            .When("Mode", 0).Advanced();

        Bind("Kick Interval", &m_kickEvery, 10, 100,
             "Ticks between dips")
            .When("Mode", 0).Advanced();

        Bind("Kick Drop", &m_kickDrop, 0.01f, 0.2f, "%.02f",
             "How far each dip falls")
            .When("Mode", 0).Advanced();

        Bind("Stop On Land", &m_stopOnLand,
             "Hands movement back once you touch the ground, so you can "
             "still jump normally")
            .When("Mode", 0).Advanced();
    }

    void OnEnable(JNIEnv*) override { m_tick = 0; }

    void OnDisable(JNIEnv* env) override {
        if (env) Stop(env, Minecraft::GetPlayer(env));
        m_tick = 0;
        m_status[0] = '\0';
    }

    void OnReset(JNIEnv* env) override {
        if (env) Stop(env, Minecraft::GetPlayer(env));
        m_tick = 0;
        m_status[0] = '\0';
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        // A screen is open: leave the player alone entirely rather
        // than freezing them in the air while they browse a chest.
        if (Minecraft::IsInGui(env)) return;

        m_tick++;

        // ---- Glide ----
        if (m_mode == 1) {
            if (Minecraft::IsOnGround(env, player)) {
                snprintf(m_status, sizeof(m_status), "grounded");
                return;
            }
            double my = Minecraft::GetMotionY(env, player);
            if (my < -(double)m_glideRate)
                Minecraft::SetMotionY(env, player, -(double)m_glideRate);
            snprintf(m_status, sizeof(m_status), "gliding");
            return;
        }

        // ---- Vanilla ----
        if (m_stopOnLand && Minecraft::IsOnGround(env, player)
            && !KeyBinds::GetJump(env)) {
            // Standing on the floor with fly on used to pin motionY
            // to zero every tick, which blocks a normal jump.
            snprintf(m_status, sizeof(m_status), "grounded");
            return;
        }

        double my = 0.0;
        if (KeyBinds::GetJump(env))  my =  m_speed * kVerticalRatio;
        if (KeyBinds::GetSneak(env)) my = -m_speed * kVerticalRatio;

        // Servers kick a player who holds a constant altitude in
        // mid-air. A brief dip resets that counter.
        if (m_antiKick && m_kickEvery > 0 && (m_tick % m_kickEvery) == 0)
            my = -(double)m_kickDrop;

        Minecraft::SetMotionY(env, player, my);
        Movement::SetHorizontal(env, player, (double)m_speed * kVerticalRatio);

        snprintf(m_status, sizeof(m_status), "flying");
    }

    NoticeLevel Notice(const char** text) const override {
        if (m_mode == 0) {
            *text = "Caught on the first airborne tick by AGC, Grim and "
                    "Polar. Unprotected servers only.";
            return NoticeLevel::Danger;
        }
        *text = "Slows your fall. Quieter than full flight, still not safe on "
                "a prediction anticheat.";
        return NoticeLevel::Warning;
    }

    const char* StatusLine() const override {
        return m_status[0] ? m_status : nullptr;
    }
};
