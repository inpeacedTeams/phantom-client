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
//    completely. It applies to EVERY mode below: Snap and Linear
//    are snapped just as Smooth is.
//
//    This applies to BOTH axes. A caller that wants pitch to move
//    at a different rate than yaw (a wrist tilts less readily than
//    it turns) must pass that ratio INTO Step via pitchRatio, not
//    scale the returned delta itself: scaling after Step has already
//    quantised puts the value straight back off the grid, which was
//    a real bug in Aim Assist and is the one thing this file exists
//    to prevent.
//
//    The quantiser CARRIES its remainder (see QuantiseCarry). A
//    plain truncation threw away any step smaller than one grid
//    quantum, which near the target is every step, so the crosshair
//    froze and then lurched a whole quantum at once. A real slow
//    mouse move does not do that: it emits one count every few
//    frames. Carrying the leftover reproduces that and is what makes
//    slow tracking smooth instead of steppy.
//
// 2. Zero jerk.
//    A hand accelerates and decelerates. Interpolating straight to
//    a target produces a velocity curve no arm can make, so Smooth
//    carries velocity between ticks and eases it instead of
//    snapping. Linear and Snap are deliberately sharper and are
//    labelled as louder where they are exposed.
//
// 3. Perfect convergence.
//    People overshoot slightly and correct. Landing exactly on
//    target every single time, forever, is its own signature, so a
//    small overshoot is baked into Smooth.
//
// ROTATION MODES
//   Smooth   ease + carried velocity + overshoot. The human-looking
//            default, and the only one meant for a real anticheat.
//   Linear   constant angular speed toward the target; `speed` is
//            read as degrees per tick. No ease, no inertia, so it
//            arrives sooner and more predictably. No overshoot.
//   Snap     the whole remaining angle in a single tick. Instant.
//            Unprotected servers only.
// =================================================================

class Rotation {
private:
    inline static float s_yawVelocity   = 0.0f;
    inline static float s_pitchVelocity = 0.0f;
    inline static float s_sensitivity   = 0.5f;   // the game's slider
    inline static bool  s_haveSens      = false;

    // Sub-grid remainder carried between ticks, one per axis. This
    // is rotation state exactly like the velocity above: it belongs
    // to the current, continuous motion toward a target and must be
    // cleared the moment that situation ends (ResetVelocity).
    inline static float s_yawResidual   = 0.0f;
    inline static float s_pitchResidual = 0.0f;

    inline static std::mt19937 s_rng{ std::random_device{}() };

public:
    struct Angles { float yaw = 0.f, pitch = 0.f; };

    // How a rotation travels toward its target. Passed to Step; the
    // grid snap and pitchRatio apply to all three.
    enum class Mode { Smooth = 0, Linear, Snap };

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

    // Snap a delta to the mouse grid, no memory. Kept for callers
    // that want a one-off quantise; the per-tick rotation path uses
    // QuantiseCarry instead so slow motion is not discarded.
    static float Quantise(float delta) {
        float g = GCD();
        if (g <= 0.0001f) return delta;
        return (float)((int)(delta / g)) * g;
    }

    // Snap to the grid while carrying the leftover into the next
    // call. Rounds to the NEAREST quantum against delta + residual,
    // then stores what did not fit. Over several ticks a stream of
    // sub-quantum deltas emits whole quanta at the right average
    // rate, which is what a slow mouse actually does, rather than
    // truncating each one to zero and lurching later.
    static float QuantiseCarry(float delta, float& residual) {
        float g = GCD();
        if (g <= 0.0001f) { residual = 0.0f; return delta; }

        float total = delta + residual;
        float steps = std::floor(total / g + 0.5f);   // nearest, not toward zero
        float out   = steps * g;
        residual    = total - out;

        // A runaway residual is impossible in normal use, but a huge
        // one-off delta (a target teleporting across the map) should
        // not leave a tail queued for later. Keep it within a
        // quantum.
        if (residual >  g) residual =  g;
        if (residual < -g) residual = -g;
        return out;
    }

    // -------------------------------------------------------------
    // Step the current rotation toward a target.
    //
    // speed        Smooth: pull strength. Linear: degrees per tick.
    //              Snap: ignored.
    // smoothing    Smooth only. 0 snaps, 1 barely moves.
    // overshoot    Smooth only. Fraction of the remaining angle to
    //              overrun by.
    // pitchRatio   how fast pitch moves relative to yaw, applied in
    //              every mode BEFORE the grid snap so the written
    //              pitch stays a real mouse value.
    // mode         Smooth, Linear or Snap. See the header comment.
    // -------------------------------------------------------------
    static Angles Step(float curYaw, float curPitch,
                       float targetYaw, float targetPitch,
                       float speed, float smoothing,
                       float jitterPct, float overshoot,
                       float pitchRatio = 1.0f,
                       Mode mode = Mode::Smooth)
    {
        float dYaw   = Wrap(targetYaw - curYaw);
        float dPitch = targetPitch - curPitch;

        float stepYaw   = 0.0f;
        float stepPitch = 0.0f;

        if (mode == Mode::Snap) {
            // The whole remaining angle at once. No velocity, no
            // ease: this is the loud one, and pretending otherwise
            // by leaving inertia around would only half-commit.
            stepYaw   = dYaw;
            stepPitch = dPitch * pitchRatio;
            s_yawVelocity = s_pitchVelocity = 0.0f;
        }
        else if (mode == Mode::Linear) {
            // Constant angular speed toward the target. `speed` is
            // degrees per tick here, not a pull factor, so the arm
            // travels at a steady rate rather than easing off as it
            // arrives. No carried velocity, so it cannot overshoot
            // and does not need the integrator.
            float maxYaw   = speed;
            float maxPitch = speed * pitchRatio;

            stepYaw = dYaw;
            if (stepYaw >  maxYaw) stepYaw =  maxYaw;
            if (stepYaw < -maxYaw) stepYaw = -maxYaw;

            stepPitch = dPitch;
            if (stepPitch >  maxPitch) stepPitch =  maxPitch;
            if (stepPitch < -maxPitch) stepPitch = -maxPitch;

            // A steady rate with zero noise is its own signature,
            // so the same per-tick jitter Smooth uses is still
            // available here.
            if (jitterPct > 0.0f) {
                float r = jitterPct * 0.01f;
                std::uniform_real_distribution<float> d(-r, r);
                stepYaw   *= (1.0f + d(s_rng));
                stepPitch *= (1.0f + d(s_rng));
            }

            s_yawVelocity = s_pitchVelocity = 0.0f;
        }
        else {
            // ---- Smooth (default) ----
            // Overshoot: aim slightly past the target while it is
            // still far away, then let it settle. People do not
            // converge perfectly and neither should this.
            if (overshoot > 0.0f) {
                float mag = std::fabs(dYaw);
                if (mag > 6.0f) {
                    std::uniform_real_distribution<float> d(0.0f, overshoot);
                    dYaw *= (1.0f + d(s_rng));
                }
            }

            // Ease toward the target rather than jumping. The closer
            // the crosshair gets, the smaller the step, which
            // removes the oscillation a plain lerp produces near
            // zero.
            float yawEase   = std::fmin(1.0f, std::fabs(dYaw)   / 28.0f);
            float pitchEase = std::fmin(1.0f, std::fabs(dPitch) / 20.0f);

            // pitchRatio is applied HERE so everything downstream —
            // the velocity carry, the jitter, and above all the grid
            // snap — acts on the value that is actually written.
            float wantYaw   = dYaw   * speed * 0.02f * yawEase;
            float wantPitch = dPitch * speed * 0.016f * pitchEase * pitchRatio;

            // Carry velocity so the arm has inertia instead of
            // teleporting.
            float k = 1.0f - smoothing;
            if (k < 0.05f) k = 0.05f;
            s_yawVelocity   += (wantYaw   - s_yawVelocity)   * k;
            s_pitchVelocity += (wantPitch - s_pitchVelocity) * k;

            stepYaw   = s_yawVelocity;
            stepPitch = s_pitchVelocity;

            if (jitterPct > 0.0f) {
                float r = jitterPct * 0.01f;
                std::uniform_real_distribution<float> d(-r, r);
                stepYaw   *= (1.0f + d(s_rng));
                stepPitch *= (1.0f + d(s_rng));
            }
        }

        // Never move further than the target: overrunning on the
        // final tick looks like a snap back. Applies to every mode.
        if (std::fabs(stepYaw)   > std::fabs(dYaw))   stepYaw   = dYaw;
        if (std::fabs(stepPitch) > std::fabs(dPitch)) stepPitch = dPitch;

        // The grid is the last thing applied, so the value written
        // is one a mouse could actually have produced. Every mode
        // passes through here, which is why none of them may be
        // scaled by the caller afterwards. The carry means a step
        // too small for one quantum this tick is not lost: it adds
        // to the next, and the crosshair advances in smooth minimum
        // steps rather than freezing and lurching.
        stepYaw   = QuantiseCarry(stepYaw,   s_yawResidual);
        stepPitch = QuantiseCarry(stepPitch, s_pitchResidual);

        Angles out;
        out.yaw   = curYaw + stepYaw;
        out.pitch = curPitch + stepPitch;

        if (out.pitch >  90.f) out.pitch =  90.f;
        if (out.pitch < -90.f) out.pitch = -90.f;

        return out;
    }

    // Rotation carries between ticks, so it has to be cleared when a
    // module stops or the arm keeps drifting after it is switched
    // off. The residuals go with it: a leftover from one target must
    // not seed the first step toward the next.
    static void ResetVelocity() {
        s_yawVelocity = 0.0f;
        s_pitchVelocity = 0.0f;
        s_yawResidual = 0.0f;
        s_pitchResidual = 0.0f;
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
