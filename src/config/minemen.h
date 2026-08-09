#pragma once

// =================================================================
// MINEMEN CLUB 1.8 — Config Profile
// =================================================================
// Anticheat: AntiGamingChair (AGC), built on Karhu source.
//
// AGC is a prediction anticheat. It simulates your movement
// server-side and compares. It also runs statistical checks on
// click intervals, rotation deltas, and packet arrival timing.
//
// It CANNOT see:
//   - client-side rendering (ESP, gamma, HUD)
//   - keyboard-level technique (w-tap, sneak, jump, blockhit)
//     because those produce packet streams identical to a human
//
// Everything below is tuned so each module's output distribution
// overlaps with what a strong human player actually produces.
// =================================================================

namespace Config {
namespace Minemen {

// ------------------------------------------------------------
// VELOCITY — legit only. Direct modes are instant flags.
// ------------------------------------------------------------
namespace Velocity {
    constexpr int   mode             = 5;      // Combined (JumpReset + Strafe)
    constexpr float jumpChance       = 65.0f;
    constexpr int   jumpDelayMin     = 0;
    constexpr int   jumpDelayMax     = 2;
    constexpr int   hitsUntilJump    = 1;
    constexpr bool  jumpOnlyGround   = true;
    constexpr float strafeStrength   = 0.55f;
    constexpr int   strafeDelay      = 1;
    constexpr bool  strafeOnlyFacing = true;

    constexpr float horizontal       = 100.0f; // Unused in legit modes
    constexpr float vertical         = 100.0f;
}

// ------------------------------------------------------------
// SPRINT RESET — biggest legit win on a duel server.
// ------------------------------------------------------------
namespace SprintReset {
    constexpr int   method           = 0;      // W-Tap
    constexpr float chance           = 88.0f;
    constexpr int   resetTicksMin    = 1;
    constexpr int   resetTicksMax    = 2;
    constexpr int   hitDelay         = 0;
    constexpr bool  onlyWhileMoving  = true;
    constexpr bool  useSTap          = false;
    // High ping: switch to method 1 (S-Tap).
    // Never method 5 (Packet): zero-gap sprint toggle pair.
}

// ------------------------------------------------------------
// AUTO BLOCKHIT — Perfect mode. Near-total coverage is fine.
// Blocking constantly is what real blockhitters do. The risk is
// in the packet cadence, not the amount, so all the safety here
// is spent on making the release timing irregular.
// ------------------------------------------------------------
namespace AutoBlockhit {
    constexpr int   mode             = 4;      // Perfect
    constexpr float coverage         = 94.0f;  // % of ticks blocked
    constexpr int   swingGapTicks    = 1;      // Minimum to let the swing land
    constexpr bool  holdBetweenHits  = true;
    constexpr int   reblockDelayMin  = 0;
    constexpr int   reblockDelayMax  = 1;

    constexpr bool  microRelease     = true;   // Essential. Breaks the held-key signature
    constexpr float microChance      = 7.0f;
    constexpr int   microLenMin      = 1;
    constexpr int   microLenMax      = 2;

    constexpr bool  varyReleaseTiming = true;
    constexpr float timingNoise      = 34.0f;  // High. Flat cadence is the only real tell

    constexpr bool  predictive       = true;
    constexpr float predictRange     = 3.6f;

    constexpr bool  onlySword        = true;   // Blocking without a sword is an instant flag
    constexpr bool  onlyWhileMoving  = false;
    constexpr bool  pauseOnFlag      = true;
    constexpr int   flagPauseTicks   = 20;
}

// ------------------------------------------------------------
// CLICK ASSIST — 20 CPS is fine, but only as butterfly.
// A flat 50ms stream at 20 CPS is not producible by a hand and
// AGC's interval statistics will say so.
// ------------------------------------------------------------
namespace ClickAssist {
    constexpr int   pattern          = 1;      // Butterfly. Mandatory above 16
    constexpr float minCPS           = 16.0f;
    constexpr float maxCPS           = 20.0f;
    constexpr bool  onlyInFight      = true;

    // Pair shape: two quick clicks, then a rest.
    // (30 + 80) / 2 per click -> ~18-20 CPS effective
    constexpr int   pairGapMin       = 24;
    constexpr int   pairGapMax       = 36;
    constexpr int   restGapMin       = 68;
    constexpr int   restGapMax       = 94;
    constexpr float pairSkipChance   = 7.0f;   // Occasional missed finger

    constexpr bool  jitter           = true;
    constexpr float jitterAmount     = 28.0f;
    constexpr bool  fatigue          = true;   // CPS sags in long fights
    constexpr float fatigueRate      = 14.0f;
    constexpr int   fatigueAfterMs   = 2400;
    constexpr bool  outliers         = true;   // Rare long gaps
    constexpr float outlierChance    = 5.0f;
    constexpr int   outlierAddMin    = 45;
    constexpr int   outlierAddMax    = 130;

    constexpr bool  breakPatterns    = true;
    constexpr float breakChance      = 9.0f;
    constexpr int   breakDuration    = 5;

    constexpr bool  entropyGuard     = true;   // Force variance if stream flattens
    constexpr float minStdDev        = 11.0f;
    constexpr int   hardFloorMs      = 24;     // Never below. Sub-20ms is impossible
}

// ------------------------------------------------------------
// BACKTRACK — usable on Minemen, but only in Pulse mode with a
// tight band. Minemen is low-ping duels, so there is very little
// natural jitter to hide inside. Keep the delay short and let it
// stop between bursts.
// ------------------------------------------------------------
namespace Backtrack {
    constexpr int   mode             = 1;      // Pulse. Constant mode is fingerprintable
    constexpr int   delayMinMs       = 35;
    constexpr int   delayMaxMs       = 70;     // Conservative for a 20ms-ping server
    constexpr int   hardCapMs        = 95;
    constexpr float perPacketJitter  = 26.0f;
    constexpr int   rampTicks        = 5;      // Grow in, never snap on

    constexpr bool  useRangeWindow   = true;
    constexpr float rangeMin         = 2.6f;   // Below this it costs you hits
    constexpr float rangeMax         = 4.2f;   // Above this it buys nothing

    constexpr int   pulseOnMin       = 5;      // Short buffer bursts
    constexpr int   pulseOnMax       = 11;
    constexpr int   pulseOffMin      = 10;     // Longer pass-through gaps
    constexpr int   pulseOffMax      = 22;

    constexpr bool  pingAware        = true;
    constexpr int   compensationMs   = 180;    // AGC window estimate, conservative
    constexpr int   safetyMarginMs   = 45;

    constexpr bool  flushOnDamage    = true;
    constexpr bool  flushOnTargetSwap = true;
    constexpr bool  onlyWhileClicking = true;  // Critical. No idle buffering
    constexpr int   pauseAfterFlagTicks = 45;
}

// ------------------------------------------------------------
// AIM ASSIST — the module most likely to get you caught, and
// the one that helps least on a duel server. Keep it slow.
// ------------------------------------------------------------
namespace AimAssist {
    constexpr float yawSpeed         = 1.6f;
    constexpr float pitchSpeed       = 1.1f;
    constexpr float fov              = 55.0f;
    constexpr float range            = 3.2f;
    constexpr int   targetMode       = 2;      // Crosshair-closest
    constexpr bool  onlyWhileClicking = true;
    constexpr bool  smoothYaw        = true;
    constexpr bool  smoothPitch      = true;
    constexpr float randomization    = 32.0f;
    constexpr bool  breakAim         = true;
    constexpr float breakChance      = 14.0f;
}

// ------------------------------------------------------------
// HIT SELECT — pure click timing. Invisible.
// ------------------------------------------------------------
namespace HitSelect {
    constexpr int   hurtTimeTarget   = 9;
    constexpr float chance           = 75.0f;
    constexpr bool  onlyInCombat     = true;
}

// ------------------------------------------------------------
// BRIDGE ASSIST — sneak is a vanilla input.
// ------------------------------------------------------------
namespace BridgeAssist {
    constexpr int   mode             = 0;      // Eagle. Godbridge toggles too fast
    constexpr float edgeDistance     = 0.26f;
    constexpr bool  onlyBackward     = true;
    constexpr bool  onlyHoldingBlocks = true;
}

namespace Sprint {
    constexpr bool  omniSprint       = false;  // Omni is a movement flag
}

// ------------------------------------------------------------
// VISUAL — never leaves the client.
// ------------------------------------------------------------
namespace ESP {
    constexpr int   boxStyle         = 0;
    constexpr bool  showBox          = true;
    constexpr bool  showHealthBar    = true;
    constexpr bool  showName         = false;
    constexpr bool  showDistance     = true;
    constexpr bool  showSnaplines    = false;
    constexpr float maxRange         = 48.0f;
}

namespace Fullbright {
    constexpr float gamma            = 100.0f;
}

// ------------------------------------------------------------
// DISABLED — Karhu's movement prediction catches all of these.
// ------------------------------------------------------------
namespace Disabled {
    constexpr bool speed        = true;   // Prediction catches any multiplier
    constexpr bool fly          = true;
    constexpr bool killAura     = true;   // Rotation + reach + timing at once
    constexpr bool noJumpDelay  = true;   // Jump frequency exceeds vanilla
}

} // namespace Minemen
} // namespace Config
