#pragma once

// =================================================================
// MINEMEN CLUB 1.8 — Config Profile
// =================================================================
// Anticheat: AntiGamingChair (AGC), built on Karhu source.
//
// What AGC/Karhu is good at:
//   - Movement PREDICTION. It simulates your movement server-side
//     and compares. Any speed/fly/timer desync = instant ban.
//   - Reach. Reportedly clean detection above ~3.05 blocks.
//     Minemen is a duel server: 1v1, low ping, no lag excuses.
//   - Velocity. Direct motion modification is trivially caught.
//   - Autoclicker. Flat CPS, low jitter entropy, perfect
//     double-click intervals all flag.
//   - Aim. Snap rotations, GCD violations, zero-jitter tracking.
//
// What AGC cannot see:
//   - Client-side rendering (ESP, gamma, HUD)
//   - Keyboard-level techniques (w-tap, sneak, jump, blockhit)
//     because they produce packet streams identical to a human
//
// Strategy: pure legit. Every module either simulates real input
// or renders locally. Nothing touches motion or reach.
// =================================================================

namespace Config {
namespace Minemen {

// ------------------------------------------------------------
// VELOCITY — legit only. Direct modes are instant flags.
// ------------------------------------------------------------
namespace Velocity {
    constexpr int   mode            = 5;      // Combined (JumpReset + Strafe)
    constexpr float jumpChance      = 65.0f;  // Not every hit. Humans miss timings
    constexpr int   jumpDelayMin    = 0;      // Randomized 0-2 ticks
    constexpr int   jumpDelayMax    = 2;
    constexpr int   hitsUntilJump   = 1;
    constexpr bool  jumpOnlyGround  = true;   // Never jump mid-air
    constexpr float strafeStrength  = 0.55f;  // Subtle. 0.8+ looks robotic
    constexpr int   strafeDelay     = 1;
    constexpr bool  strafeOnlyFacing = true;

    // Direct-mode values are intentionally left at vanilla so that
    // switching modes by accident does nothing dangerous.
    constexpr float horizontal      = 100.0f;
    constexpr float vertical        = 100.0f;
}

// ------------------------------------------------------------
// SPRINT RESET — the single biggest legit win on Minemen.
// Pure keyboard simulation. AGC sees a normal w-tapper.
// ------------------------------------------------------------
namespace SprintReset {
    constexpr int   method          = 0;      // W-Tap. Safest, works everywhere
    constexpr float chance          = 88.0f;  // Not 100%. Humans miss resets
    constexpr int   resetTicksMin   = 1;
    constexpr int   resetTicksMax   = 2;      // Randomized duration
    constexpr int   hitDelay        = 0;
    constexpr bool  onlyWhileMoving = true;
    constexpr bool  useSTap         = false;

    // Alternative for high-ping players: method 1 (S-Tap).
    // Alternative when holding a sword: method 2 (Blockhit).
    // NEVER use method 5 (Packet) here: it produces a
    // sprint-toggle packet pair with zero tick gap.
}

// ------------------------------------------------------------
// AUTO BLOCKHIT — safe, but must not fire on every single hit.
// ------------------------------------------------------------
namespace AutoBlockhit {
    constexpr int   mode            = 1;      // Timed, not Normal
    constexpr float chance          = 70.0f;
    constexpr int   blockTicksMin   = 1;
    constexpr int   blockTicksMax   = 3;      // Wide range = human
    constexpr int   hitIntervalMin  = 1;
    constexpr int   hitIntervalMax  = 3;      // Block every 1-3 hits
    constexpr bool  onlySword       = true;
    constexpr bool  onlyWhileMoving = true;
}

// ------------------------------------------------------------
// AIM ASSIST — the most dangerous legit module. Go slow.
// AGC checks rotation deltas, GCD alignment, and jitter entropy.
// ------------------------------------------------------------
namespace AimAssist {
    constexpr float yawSpeed        = 1.6f;   // Very low. 4.0+ is visible in replays
    constexpr float pitchSpeed      = 1.1f;   // Pitch slower than yaw, like a human
    constexpr float fov             = 55.0f;  // Narrow cone. 120 tracks through walls
    constexpr float range           = 3.2f;   // Under vanilla reach
    constexpr int   targetMode      = 2;      // Crosshair-closest, not auto-closest
    constexpr bool  onlyWhileClicking = true; // Critical. No idle tracking
    constexpr bool  smoothYaw       = true;
    constexpr bool  smoothPitch     = true;
    constexpr float randomization   = 32.0f;  // High jitter. Kills GCD patterns
    constexpr bool  breakAim        = true;
    constexpr float breakChance     = 14.0f;  // Skips ticks. Creates natural misses
}

// ------------------------------------------------------------
// HIT SELECT — pure click timing. Invisible to AGC.
// ------------------------------------------------------------
namespace HitSelect {
    constexpr int   hurtTimeTarget  = 9;
    constexpr float chance          = 75.0f;
    constexpr bool  onlyInCombat    = true;
}

// ------------------------------------------------------------
// CLICK ASSIST — Minemen watches CPS distribution closely.
// Keep the ceiling under 16 and the curve messy.
// ------------------------------------------------------------
namespace ClickAssist {
    constexpr float minCPS          = 9.0f;
    constexpr float maxCPS          = 14.0f;  // Hard ceiling. 16+ draws staff attention
    constexpr bool  onlyInFight     = true;
    constexpr bool  jitter          = true;
    constexpr float jitterAmount    = 28.0f;  // Heavy. Flat intervals are the tell
    constexpr bool  breakPatterns   = true;
    constexpr float breakChance     = 12.0f;
    constexpr int   breakDuration   = 4;
}

// ------------------------------------------------------------
// BRIDGE ASSIST — sneak is a legit input. Safe.
// Godbridge mode toggles sneak too fast, so stick to Eagle.
// ------------------------------------------------------------
namespace BridgeAssist {
    constexpr int   mode            = 0;      // Eagle. Not Godbridge
    constexpr float edgeDistance    = 0.26f;
    constexpr bool  onlyBackward    = true;
    constexpr bool  onlyHoldingBlocks = true;
}

// ------------------------------------------------------------
// SPRINT — always-sprint. Vanilla-identical packet stream.
// ------------------------------------------------------------
namespace Sprint {
    constexpr bool  omniSprint      = false;  // Forward-only. Omni is a movement flag
}

// ------------------------------------------------------------
// VISUAL — never leaves the client. Zero risk.
// ------------------------------------------------------------
namespace ESP {
    constexpr int   boxStyle        = 0;      // 2D Corners
    constexpr bool  showBox         = true;
    constexpr bool  showHealthBar   = true;
    constexpr bool  showName        = false;  // Nametags already exist in 1v1
    constexpr bool  showDistance    = true;
    constexpr bool  showSnaplines   = false;
    constexpr float maxRange        = 48.0f;
}

namespace Fullbright {
    constexpr float gamma           = 100.0f;
}

// ------------------------------------------------------------
// DISABLED MODULES
// ------------------------------------------------------------
// Karhu's movement prediction simulates your position every tick.
// These modules produce a desync it cannot explain as lag.
//
//   Speed        — instant. Prediction catches any multiplier
//   Fly          — instant
//   KillAura     — rotation + reach + timing, all at once
//   NoJumpDelay  — jump frequency exceeds vanilla possibility
//   Backtrack    — AGC tracks packet arrival intervals
//
// Backtrack CAN be run at 40-60ms if you accept the risk, but on
// a low-ping duel server the delay is visible in your own hits.
// Recommended: off.
// ------------------------------------------------------------
namespace Disabled {
    constexpr bool speed        = true;
    constexpr bool fly          = true;
    constexpr bool killAura     = true;
    constexpr bool noJumpDelay  = true;
    constexpr bool backtrack    = true;
}

} // namespace Minemen
} // namespace Config
