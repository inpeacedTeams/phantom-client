#pragma once
#include <jni.h>
#include <cmath>
#include <random>
#include <deque>

#include "minecraft.h"

// =================================================================
// Rotation
// =================================================================
// Everything that turns the player goes through here, because the
// way a rotation is produced matters far more than how fast it is.
//
// THREE THINGS GIVE AWAY A MACHINE-MADE ROTATION
//
// 1. GCD violation.
//    A real mouse cannot produce arbitrary angles. The client
//    multiplies raw counts by a sensitivity factor, so every yaw a
//    human generates is an exact multiple of that step:
//
//        f = sens * 0.6 + 0.2
//        step = f^3 * 1.2
//
//    Writing 47.8213 degrees when the smallest possible increment
//    is 0.0439 is arithmetically impossible for a player. Checking
//    this costs an anticheat nothing and catches nearly every
//    naive aim assist. Quantising to the step removes the tell
//    completely.
//
// 2. Zero jerk.
//    A hand accelerates and decelerates. Interpolating straight to
//    a target produces a velocity curve no arm can make, so the
//    engine carries velocity between ticks and eases it instead of
//    snapping.
//
// 3. Perfect convergence.
//    People overshoot slightly and correct. Landing exactly on
//    target every single time, forever, is its own signature, so a
//    small overshoot is baked in.
// =================================================================

class Rotation {
private:
    inline static float s_yawVelocity   = 0.0f;
    inline static float s_pitchVelocity = 0.0f;
    inline static float s_sensitivity   = 0.5f;   // the game's slider
    inline static bool  s_haveSens      = false;

    inline static std::mt19937 s_rng{ std::random_device{}() };

public:
    struct Angles { float yaw = 0.f, pitch = 0.f; };

    static float Wrap(float a) {
        while (a > 180.f)  a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    }

    // The smallest rotation step this player's sensitivity allows.
    // Anything not a multiple of it is impossible with a mouse.
    static float GCD() {
        float f = s_sensitivity * 0.6f + 0.2f;
        return f * f * f * 1.2f;
    }

    static void SetSensitivity(float s) {
        s_sensitivity = s;
        s_haveSens = true;
    }

    static bool HaveSensitivity() { return s_haveSens; }
    static float Sensitivity() { return s_sensitivity; }

    // Snap a delta to the mouse grid. Called on the DELTA rather
    // than the absolute angle, because that is what the client
    // itself quantises.
    static float Quantise(float delta) {
        float g = GCD();
        if (g <= 0.0001f) return delta;
        return (float)((int)(delta / g)) * g;
    }

    // -------------------------------------------------------------
    // Step the current rotation toward a target.
    //
    // speed        degrees per tick at full commitment
    // smoothing    0 snaps, 1 barely moves
    // overshoot    fraction of the remaining angle to overrun by
    // -------------------------------------------------------------
    static Angles Step(float curYaw, float curPitch,
                       float targetYaw, float targetPitch,
                       float speed, float smoothing,
                       float jitterPct, float overshoot)
    {
        float dYaw   = Wrap(targetYaw - curYaw);
        float dPitch = targetPitch - curPitch;

        // Overshoot: aim slightly past the target while it is still
        // far away, then let it settle. People do not converge
        // perfectly and neither should this.
        if (overshoot > 0.0f) {
            float mag = std::fabs(dYaw);
            if (mag > 6.0f) {
                std::uniform_real_distribution<float> d(0.0f, overshoot);
                dYaw *= (1.0f + d(s_rng));
            }
        }

        // Ease toward the target rather than jumping. The closer the
        // crosshair gets, the smaller the step, which removes the
        // oscillation a plain lerp produces near zero.
        float yawEase   = std::fmin(1.0f, std::fabs(dYaw)   / 28.0f);
        float pitchEase = std::fmin(1.0f, std::fabs(dPitch) / 20.0f);

        float wantYaw   = dYaw   * speed * 0.02f * yawEase;
        float wantPitch = dPitch * speed * 0.016f * pitchEase;

        // Carry velocity so the arm has inertia instead of teleporting
        float k = 1.0f - smoothing;
        if (k < 0.05f) k = 0.05f;
        s_yawVelocity   += (wantYaw   - s_yawVelocity)   * k;
        s_pitchVelocity += (wantPitch - s_pitchVelocity) * k;

        float stepYaw   = s_yawVelocity;
        float stepPitch = s_pitchVelocity;

        if (jitterPct > 0.0f) {
            float r = jitterPct * 0.01f;
            std::uniform_real_distribution<float> d(-r, r);
            stepYaw   *= (1.0f + d(s_rng));
            stepPitch *= (1.0f + d(s_rng));
        }

        // Never move further than the target: overrunning on the
        // final tick looks like a snap back.
        if (std::fabs(stepYaw)   > std::fabs(dYaw))   stepYaw   = dYaw;
        if (std::fabs(stepPitch) > std::fabs(dPitch)) stepPitch = dPitch;

        // The grid is the last thing applied, so the value written
        // is one a mouse could actually have produced.
        stepYaw   = Quantise(stepYaw);
        stepPitch = Quantise(stepPitch);

        Angles out;
        out.yaw   = curYaw + stepYaw;
        out.pitch = curPitch + stepPitch;

        if (out.pitch >  90.f) out.pitch =  90.f;
        if (out.pitch < -90.f) out.pitch = -90.f;

        return out;
    }

    // Rotation carries between ticks, so it has to be cleared when a
    // module stops or the arm keeps drifting after it is switched off.
    static void ResetVelocity() {
        s_yawVelocity = 0.0f;
        s_pitchVelocity = 0.0f;
    }

    static float YawVelocity()   { return s_yawVelocity; }
    static float PitchVelocity() { return s_pitchVelocity; }

    // Aim point on a target's body. Feet are 0, eyes are about 1.62.
    // Aiming dead centre every time is itself a pattern, so callers
    // pass a randomised height.
    static Angles ToPoint(JNIEnv* env, jobject player,
                          double tx, double ty, double tz)
    {
        auto r = Minecraft::GetRotationsToPos(env, player, tx, ty, tz);
        Angles a;
        a.yaw = r.yaw;
        a.pitch = r.pitch;
        return a;
    }
};
