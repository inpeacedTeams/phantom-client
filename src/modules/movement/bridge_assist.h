#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/movement.h"
#include "../../mc/world.h"
#include <Windows.h>
#include <cmath>
#include <cstdio>

// =================================================================
// Bridge Assist (Eagle)
// =================================================================
// One job: hold shift while you are hanging over the drop, and let
// go the moment you have placed a block, so you can walk back onto
// it and repeat. That loop is the whole of eagle bridging.
//
// -----------------------------------------------------------------
// WHY THE OLD ONE MISBEHAVED
// -----------------------------------------------------------------
// It was a pure edge sensor: shift went down whenever there was air
// ahead and came up whenever there was not. Nothing in it knew that
// a block had been PLACED.
//
// That fails in the exact rhythm of bridging. You place a block, and
// for a moment the ground behind you is solid AND you are still over
// the seam, so the sensor flickers shift on and off several times a
// second. Each toggle is a sneak packet and a movement stutter, and
// because sneak also caps your speed you end up crawling. The four
// modes on top of that were four different ways of flickering.
//
// -----------------------------------------------------------------
// HOW IT WORKS NOW
// -----------------------------------------------------------------
// A small state machine with one real event in it: the block landed.
//
//     WALKING -- air ahead --> HELD -- block placed --> SETTLE
//        ^                                               |
//        +------------ press delay <-- RECOVER <---------+
//
//   WALKING   ground ahead, shift is yours again
//   HELD      shift down, you are over the gap, waiting for a block
//   SETTLE    a block landed, counting down before letting go
//   RECOVER   shift up, you are stepping back onto it
//
// Placement is detected from the world, not from the mouse: the cell
// you were about to step into goes from air to solid. That is the
// only signal that cannot lie. Right-click is a fallback for when
// the block lookup could not be resolved, because a click is not a
// placement (you may have hit a wall, or run out of blocks).
//
// -----------------------------------------------------------------
// PACE
// -----------------------------------------------------------------
// Three numbers, and each one moves the speed for a different
// reason. This is not a pile of sliders: it is the three moments in
// the cycle where you can spend or save time.
//
//   Edge Distance   how far out you get before shift goes down.
//   Release Delay   ticks between the block landing and shift
//                   letting go.
//   Press Delay     ticks after releasing before shift may go down
//                   again. This is what stops the flicker.
//
// The preset sets all three at once. Touch any of them and the
// preset says Custom, because a preset that claims to be Balanced
// while holding somebody else's numbers is a lie on screen.
// =================================================================

class BridgeAssist : public Module {
private:
    enum class State { Walking, Held, Settle, Recover };

    enum Pace { SAFE = 0, BALANCED, FAST, CUSTOM };

    static constexpr const char* kPace[] = { "Safe", "Balanced", "Fast", "Custom" };

    // ---- Pace ----
    // Mutable because the preset reconciles itself while the panel
    // is open, and the panel only ever calls const methods. See
    // SyncPreset.
    mutable int   m_preset       = BALANCED;
    mutable float m_edgeDistance = 0.30f;
    mutable int   m_releaseDelay = 1;
    mutable int   m_pressDelay   = 2;
    mutable int   m_lastPreset   = BALANCED;

    // ---- Behaviour ----
    bool  m_onlyBackward = true;
    bool  m_onlyOnGround = true;
    bool  m_holdOnStop   = true;   // keep shift while standing still over air

    // ---- Advanced ----
    bool  m_useWorld     = true;   // real block test when available
    float m_edgeFallback = 0.28f;  // fractional guess if it is not
    int   m_maxHoldTicks = 60;     // safety valve on a stuck hold

    // ---- State ----
    State m_state       = State::Walking;
    bool  m_sneaking    = false;
    int   m_settleLeft  = 0;
    int   m_recoverLeft = 0;
    int   m_heldTicks   = 0;

    // Placement detection
    int   m_watchX = 0, m_watchY = 0, m_watchZ = 0;
    bool  m_watching = false;
    bool  m_watchWasSolid = false;
    bool  m_lastUse = false;

    // Readout
    bool  m_worldOk = false;
    int   m_placed  = 0;
    const char* m_why = "idle";
    mutable char m_status[48] = {};

    static const char* StateName(State s) {
        switch (s) {
            case State::Held:    return "holding";
            case State::Settle:  return "block down";
            case State::Recover: return "stepping back";
            default:             return "walking";
        }
    }

    struct PaceValues { float edge; int release; int press; };

    static PaceValues ValuesFor(int preset) {
        switch (preset) {
            case SAFE:     return { 0.38f, 2, 3 };   // crouch early, let go late
            case FAST:     return { 0.18f, 0, 1 };   // hang out as far as possible
            default:       return { 0.30f, 1, 2 };   // balanced
        }
    }

    // Keeps the preset and the three timings honest with each other,
    // in both directions:
    //
    //   pick a preset  -> the timings become its numbers
    //   move a timing  -> the preset becomes Custom
    //
    // Done here rather than in a widget callback because no module
    // draws its own panel any more, and this has to keep working
    // when a config file sets the values directly.
    void SyncPreset() const {
        if (m_preset != m_lastPreset) {
            if (m_preset != CUSTOM) {
                PaceValues v = ValuesFor(m_preset);
                m_edgeDistance = v.edge;
                m_releaseDelay = v.release;
                m_pressDelay   = v.press;
            }
            m_lastPreset = m_preset;
            return;
        }

        if (m_preset == CUSTOM) return;

        PaceValues v = ValuesFor(m_preset);
        bool matches = std::fabs(m_edgeDistance - v.edge) < 0.005f
                    && m_releaseDelay == v.release
                    && m_pressDelay   == v.press;
        if (!matches) {
            m_preset = CUSTOM;
            m_lastPreset = CUSTOM;
        }
    }

    // Sneak is re-asserted every tick rather than only on a change.
    // Another module drives the same bind (Sprint Reset in Sneak Tap
    // mode), and when it hands the key back it restores the hardware
    // state, which is "shift is not held". Caching our own idea of
    // the key meant we never noticed and walked off the edge.
    void ApplySneak(JNIEnv* env, bool on) {
        if (!env) { m_sneaking = false; return; }
        if (on) KeyBinds::SetSneak(env, true);
        else if (m_sneaking) KeyBinds::ReleaseSneak(env);
        m_sneaking = on;
    }

    // Is the ground about to run out in the direction we are going?
    // Also reports the cell that would be empty, which is the one we
    // then watch for a block appearing in.
    bool EdgeAhead(JNIEnv* env, jobject player, bool moving,
                   double dirX, double dirZ)
    {
        double px = Minecraft::GetPosX(env, player);
        double py = Minecraft::GetPosY(env, player);
        double pz = Minecraft::GetPosZ(env, player);

        // Standing still: the question is whether the block under our
        // own feet is still there, which is exactly the case where
        // letting go would drop you.
        double ax = moving ? px + dirX * m_edgeDistance : px;
        double az = moving ? pz + dirZ * m_edgeDistance : pz;

        int bx = (int)std::floor(ax);
        int by = (int)std::floor(py) - 1;
        int bz = (int)std::floor(az);

        bool edge;
        if (m_useWorld && m_worldOk) {
            edge = !World::IsSolid(env, bx, by, bz);
        } else {
            // No block lookup: fall back to the fractional guess. It
            // is wrong on a flat floor, which is why it is a fallback.
            double fx = ax - std::floor(ax);
            double fz = az - std::floor(az);
            double d = (double)m_edgeFallback;
            edge = (fx < d) || (fx > 1.0 - d) || (fz < d) || (fz > 1.0 - d);
        }

        if (edge) {
            m_watchX = bx; m_watchY = by; m_watchZ = bz;
            m_watching = true;
            m_watchWasSolid = false;
        }
        return edge;
    }

    // Did a block just land in the cell we were hanging over?
    bool BlockLanded(JNIEnv* env) {
        if (m_useWorld && m_worldOk && m_watching) {
            bool solid = World::IsSolid(env, m_watchX, m_watchY, m_watchZ);
            bool landed = solid && !m_watchWasSolid;
            m_watchWasSolid = solid;
            if (landed) m_placed++;
            return landed;
        }

        // Fallback: the rising edge of right-click. Weaker, because a
        // click is not a placement, but better than never releasing.
        bool use = KeyBinds::GetUseItem(env);
        bool edge = use && !m_lastUse;
        m_lastUse = use;
        if (edge) m_placed++;
        return edge;
    }

    void GoWalking(JNIEnv* env) {
        ApplySneak(env, false);
        m_state = State::Walking;
        m_watching = false;
        m_heldTicks = 0;
    }

public:
    BridgeAssist()
        : Module("Bridge Assist", "Eagle bridging: hold shift over the gap, "
                                  "let go once the block lands",
                 ModuleCategory::MOVEMENT, 0)
    {
        BindMode("Pace", &m_preset, kPace, 4,
                 "Safe crouches early and lets go late. Fast hangs as far "
                 "out as it can. Touch any timing below and this becomes "
                 "Custom.");

        Bind("Edge Distance", &m_edgeDistance, 0.10f, 0.45f, "%.2f",
             "How far out you get before shift goes down. The single "
             "biggest lever on how fast the bridge grows.");

        Bind("Release Delay", &m_releaseDelay, 0, 5,
             "Ticks the block has to settle before shift lets go");

        Bind("Press Delay", &m_pressDelay, 0, 6,
             "Ticks to walk back on before shift may re-engage. Too low and "
             "it stutters on the seam.");

        Bind("Only While Walking Backwards", &m_onlyBackward,
             "Eagle is a backwards technique. Off means it also catches you "
             "walking forwards off a ledge.")
            .Advanced();

        Bind("Hold While Standing Still", &m_holdOnStop,
             "Keeps shift down if you stop with your heels over the drop")
            .Advanced();

        Bind("Only On Ground", &m_onlyOnGround,
             "Do nothing while you are in the air")
            .Advanced();

        Bind("Use Block Lookup", &m_useWorld,
             "Read the world, so a placement is a real block appearing "
             "rather than a click that may have hit nothing")
            .Advanced();

        Bind("Edge Fallback", &m_edgeFallback, 0.05f, 0.49f, "%.2f",
             "Fractional guess used when the world cannot be read")
            .Advanced();

        Bind("Max Hold", &m_maxHoldTicks, 20, 120,
             "Gives the key back if no block ever arrives, so running out of "
             "materials does not pin you in place")
            .Advanced();
    }

    void OnEnable(JNIEnv*) override {
        m_state = State::Walking;
        m_watching = false;
        m_settleLeft = m_recoverLeft = m_heldTicks = 0;
        m_placed = 0;
    }

    void OnDisable(JNIEnv* env) override {
        if (env) {
            ApplySneak(env, false);
            KeyBinds::ReleaseSneak(env);   // backstop
        }
        m_state = State::Walking;
        m_watching = false;
        m_settleLeft = m_recoverLeft = m_heldTicks = 0;
        m_why = "off";
        m_status[0] = '\0';
    }

    // A world change mid-hold would otherwise carry shift, and a
    // watched cell from the old map, straight into the new one.
    void OnReset(JNIEnv* env) override {
        if (env) KeyBinds::ReleaseSneak(env);
        m_sneaking = false;
        m_state = State::Walking;
        m_watching = false;
        m_watchWasSolid = false;
        m_lastUse = false;
        m_settleLeft = m_recoverLeft = m_heldTicks = 0;
        m_placed = 0;
        m_why = "idle";
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;
        SyncPreset();
        m_worldOk = World::Init(env);

        jobject player = Minecraft::GetPlayer(env);

        // Losing the player mid-hold used to leave shift down all the
        // way into the next world.
        if (!player) { GoWalking(env); m_why = "no player"; return; }

        if (Minecraft::IsInGui(env)) { GoWalking(env); m_why = "menu"; return; }

        if (m_onlyOnGround && !Minecraft::IsOnGround(env, player)) {
            GoWalking(env);
            m_why = "airborne";
            return;
        }

        // Shared vanilla movement maths rather than a fourth private
        // copy of it. Read from the keys, not the motion vector:
        // motion is already zero on the tick you would step off.
        double dirX = 0, dirZ = 0;
        bool moving = Movement::Direction(env, player, &dirX, &dirZ);

        // Eagle is a backwards technique. Walking forwards off a
        // ledge is just falling, and crouching for it helps nobody.
        if (m_onlyBackward && moving && !KeyBinds::GetBack(env)) {
            GoWalking(env);
            m_why = "not bridging";
            return;
        }

        // ---- The cycle ----
        switch (m_state) {

        case State::Walking: {
            if (!moving && !m_holdOnStop) { GoWalking(env); m_why = "still"; break; }

            if (EdgeAhead(env, player, moving, dirX, dirZ)) {
                ApplySneak(env, true);
                m_state = State::Held;
                m_heldTicks = 0;
                m_why = "over the gap";
            } else {
                ApplySneak(env, false);
                m_why = "ground ahead";
            }
            break;
        }

        case State::Held: {
            // Shift stays down for the whole of this state. No
            // re-evaluating the edge, which is what caused the
            // flicker: the answer changes twice a second while you
            // shuffle around on the seam.
            ApplySneak(env, true);
            m_heldTicks++;

            if (BlockLanded(env)) {
                m_state = State::Settle;
                m_settleLeft = m_releaseDelay;
                m_why = "block landed";
                break;
            }

            // Safety valve. If we have been crouched for three
            // seconds no block is coming: out of materials, or
            // aiming at nothing.
            if (m_heldTicks > m_maxHoldTicks) {
                GoWalking(env);
                m_why = "nothing placed, released";
                break;
            }

            m_why = "waiting for a block";
            break;
        }

        case State::Settle: {
            // The block is down but the server may not have it yet,
            // so shift is held a beat longer.
            if (m_settleLeft > 0) {
                ApplySneak(env, true);
                m_settleLeft--;
                m_why = "settling";
                break;
            }
            ApplySneak(env, false);
            m_state = State::Recover;
            m_recoverLeft = m_pressDelay;
            m_why = "released";
            break;
        }

        case State::Recover: {
            // Shift is up and you are walking back onto the block we
            // just placed. Not testing the edge here is the point:
            // you are standing right on the seam and the test would
            // immediately send us back to Held.
            ApplySneak(env, false);
            if (m_recoverLeft > 0) {
                m_recoverLeft--;
                m_why = "stepping back";
                break;
            }
            m_state = State::Walking;
            m_watching = false;
            m_why = "ready";
            break;
        }

        }
    }

    const char* StatusLine() const override {
        if (!m_sneaking && m_state == State::Walking) return nullptr;
        snprintf(m_status, sizeof(m_status), "%s \xc2\xb7 %d placed",
                 StateName(m_state), m_placed);
        return m_status;
    }

    NoticeLevel Notice(const char** text) const override {
        // The panel is the other place the preset can be changed, and
        // this is the one hook that runs while it is open.
        SyncPreset();

        if (!KeyBinds::HasSneak()) {
            *text = "The sneak keybind could not be found in this build of "
                    "the game, so nothing can be held.";
            return NoticeLevel::Danger;
        }
        if (m_useWorld && !m_worldOk) {
            *text = "The world could not be read, so a right-click is being "
                    "treated as a placement. That is weaker: a click into a "
                    "wall counts too.";
            return NoticeLevel::Warning;
        }
        return NoticeLevel::None;
    }
};
