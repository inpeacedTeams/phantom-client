#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../mc/rotation.h"
#include "../../input/focus.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <Windows.h>
#include <cmath>
#include <random>
#include <string>
#include <cstdio>

// =================================================================
// Aim Assist
// =================================================================
// Nudges the crosshair toward a target. Every rotation goes through
// the shared Rotation engine, which quantises to the mouse grid,
// carries velocity between ticks and adds a little overshoot.
//
// WHAT ACTUALLY GETS AIM ASSIST CAUGHT
//
// Not speed. A fast flick is perfectly human. What gets caught is:
//
//   * angles that are not multiples of the sensitivity step, which
//     no mouse can physically produce
//   * tracking that never breaks, never overshoots and never loses
//     the target for a moment
//   * aiming at the exact centre of the hitbox every time
//   * assistance while you are not even fighting
//
// So: the grid is enforced, the aim point wanders around the body,
// targets are sticky rather than re-picked every tick, and the
// whole thing only runs while you are actually swinging.
//
// PITCH RATIO lives inside Rotation::Step now, not here. Scaling the
// pitch that Step returned used to knock it straight back off the
// mouse grid, which is the one thing this module must never do.
//
// ROTATION MODE picks the travel shape (Smooth, Linear, Snap) and
// is passed straight into Step, so the grid snap covers all three.
// Smooth is the human-looking default; the other two are faster and
// louder, and the notice says so.
// =================================================================

class AimAssist : public Module {
private:
    static constexpr const char* kPriorities[] = {
        "Crosshair", "Closest", "Lowest HP"
    };

    static constexpr const char* kRotModes[] = {
        "Smooth", "Linear", "Snap"
    };

    // Past this the correction is visible when someone watches a
    // recording of the fight frame by frame.
    static constexpr float kVisibleSpeed = 6.0f;

    // How often the aim point is re-rolled, in ticks. Every tick
    // reads as noise rather than drift.
    static constexpr int kWanderPeriod = 7;

    // ---- Feel ----
    int   m_rotMode    = 0;      // 0 smooth, 1 linear, 2 snap
    float m_speed      = 3.2f;
    float m_pitchRatio = 0.6f;   // pitch moves slower than yaw, like a wrist
    float m_smoothing  = 0.55f;
    float m_fov        = 70.0f;
    float m_range      = 3.6f;

    // ---- Targeting ----
    int   m_targetMode = 0;      // 0 crosshair, 1 closest, 2 lowest HP
    bool  m_sticky     = true;   // keep the current target while valid
    float m_stickyFov  = 110.0f; // give it up past this angle
    bool  m_requireSwing = true; // only while actually fighting
    int   m_swingWindow  = 8;    // ticks after a swing to keep helping

    // ---- Aim point ----
    float m_aimHeight = 1.1f;    // metres up the body
    float m_wander    = 0.22f;   // how far it drifts around that point

    // ---- Humanisation ----
    float m_jitter    = 14.0f;
    float m_overshoot = 0.06f;
    bool  m_breaks    = true;
    float m_breakChance = 6.0f;
    int   m_breakMin  = 1;
    int   m_breakMax  = 3;

    // ---- State ----
    int    m_targetId = -1;
    int    m_breakLeft = 0;
    int    m_ticksOnTarget = 0;
    double m_wanderX = 0, m_wanderY = 0, m_wanderZ = 0;
    int    m_wanderTick = 0;

    // Readout
    std::string m_targetName;
    float m_lastAngle = 0.0f;
    bool  m_active = false;
    mutable char m_status[80] = {};
    mutable char m_notice[176] = {};

    jmethodID m_getEntityId = nullptr;
    jfieldID  m_fSens = nullptr;
    bool m_resolved = false;

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
    float Randf(float lo, float hi) {
        std::uniform_real_distribution<float> d(lo, hi);
        return d(m_rng);
    }

    // Map the setting index onto the engine enum in one place.
    Rotation::Mode RotMode() const {
        switch (m_rotMode) {
            case 1:  return Rotation::Mode::Linear;
            case 2:  return Rotation::Mode::Snap;
            default: return Rotation::Mode::Smooth;
        }
    }

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;

        if (ClassResolver::entity) {
            m_getEntityId = JvmtiUtil::FindMethod(env, ClassResolver::entity,
                { "func_145782_y", "getEntityId" }, 0);
        }

        // The mouse grid depends on the player's own sensitivity, so
        // it has to be read rather than assumed.
        if (ClassResolver::gameSettings) {
            m_fSens = JvmtiUtil::FindField(env, ClassResolver::gameSettings,
                { "field_74341_c", "mouseSensitivity" });
        }
        m_resolved = true;
    }

    void RefreshSensitivity(JNIEnv* env) {
        if (!m_fSens) return;
        jobject gs = Minecraft::GetGameSettings(env);
        if (!gs) return;
        Rotation::SetSensitivity(env->GetFloatField(gs, m_fSens));
    }

    int EntityId(JNIEnv* env, jobject ref, const std::string& name) {
        if (m_getEntityId) {
            jint v = env->CallIntMethod(ref, m_getEntityId);
            if (!env->ExceptionCheck()) return (int)v;
            env->ExceptionClear();
        }
        return (int)std::hash<std::string>{}(name);
    }

    float AngleTo(JNIEnv* env, jobject player,
                  double x, double y, double z, float yaw, float pitch)
    {
        auto r = Minecraft::GetRotationsToPos(env, player, x, y, z);
        float dy = Rotation::Wrap(r.yaw - yaw);
        float dp = r.pitch - pitch;
        return std::sqrt(dy * dy + dp * dp);
    }

public:
    AimAssist() : Module("Aim Assist", "Guides the crosshair while you fight",
                         ModuleCategory::COMBAT, 'R')
    {
        BindMode("Priority", &m_targetMode, kPriorities, 3,
                 "Which of several people in reach it helps you with");

        BindMode("Rotation", &m_rotMode, kRotModes, 3,
                 "Smooth eases in with inertia and reads as a hand. Linear "
                 "pulls at a steady speed and arrives sooner. Snap turns "
                 "the whole way in one tick and is for unprotected servers.");

        Bind("Speed", &m_speed, 0.5f, 10.0f, "%.1f",
             "How hard it pulls toward the target. Under Linear this is read "
             "as degrees per tick.");

        Bind("FOV", &m_fov, 10.0f, 180.0f, "%.0f",
             "How far off centre someone can be and still be picked up");

        Bind("Range", &m_range, 1.0f, 6.0f, "%.1f",
             "How far away it still helps");

        // ---- Feel ----
        Bind("Pitch Ratio", &m_pitchRatio, 0.1f, 1.0f, "%.2f",
             "Wrists turn sideways far more readily than they tilt, and "
             "matching that is most of why a rotation reads as human")
            .Advanced();

        // Smoothing is the Smooth curve's inertia. Linear has none by
        // definition and Snap is instant, so it only applies to mode 0.
        Bind("Smoothing", &m_smoothing, 0.0f, 0.9f, "%.2f",
             "How much of last tick's movement carries into this one")
            .When("Rotation", 0).Advanced();

        // ---- Targeting ----
        Bind("Sticky", &m_sticky,
             "Keep the current target. Switching mid-exchange produces a "
             "whip-round that nothing human does.")
            .Advanced();

        Bind("Sticky FOV", &m_stickyFov, 40.0f, 200.0f, "%.0f",
             "Give the target up past this angle")
            .When("Sticky", 1).Advanced();

        Bind("Only While Swinging", &m_requireSwing,
             "Tracking someone across a lobby while you stand still is the "
             "loudest thing this module can do")
            .Advanced();

        Bind("Swing Window", &m_swingWindow, 2, 20,
             "Ticks after a swing that it keeps helping")
            .When("Only While Swinging", 1).Advanced();

        // ---- Aim point ----
        Bind("Aim Height", &m_aimHeight, 0.0f, 1.8f, "%.2f",
             "Metres up the body it aims for")
            .Advanced();

        Bind("Wander", &m_wander, 0.0f, 0.5f, "%.2f",
             "How far the aim point drifts. Near zero means perfect centre "
             "aim on every single hit.")
            .Advanced();

        // ---- Humanisation ----
        Bind("Jitter", &m_jitter, 0.0f, 40.0f, "%.0f%%",
             "Noise on each step, on top of the mouse grid")
            .Advanced();

        // Overshoot is a Smooth-only behaviour: Linear is capped at
        // the remaining angle and Snap lands exactly on target, so
        // neither can overrun.
        Bind("Overshoot", &m_overshoot, 0.0f, 0.25f, "%.2f",
             "How often it goes slightly past and corrects back")
            .When("Rotation", 0).Advanced();

        Bind("Breaks", &m_breaks,
             "Deliberate lapses. Perfect tracking forever is a signature in "
             "itself.")
            .Advanced();

        Bind("Break Chance", &m_breakChance, 1.0f, 25.0f, "%.0f%%")
            .When("Breaks", 1).Advanced();

        Bind("Break Min", &m_breakMin, 1, 8,
             "Ticks a lapse lasts")
            .When("Breaks", 1).Advanced();

        Bind("Break Max", &m_breakMax, 1, 12)
            .When("Breaks", 1).Advanced();
    }

    void OnEnable(JNIEnv*) override {
        Rotation::ResetVelocity();
        m_targetId = -1;
        m_breakLeft = 0;
        m_ticksOnTarget = 0;
    }

    void OnDisable(JNIEnv*) override {
        Rotation::ResetVelocity();
        m_targetId = -1;
        m_targetName.clear();
        m_active = false;
        m_status[0] = '\0';
    }

    // The target id belongs to the world that just went away, and
    // holding it means the first entity to reuse that id gets aimed
    // at for no reason.
    void OnReset(JNIEnv*) override {
        Rotation::ResetVelocity();
        m_targetId = -1;
        m_targetName.clear();
        m_breakLeft = 0;
        m_ticksOnTarget = 0;
        m_active = false;
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);
        m_active = false;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        if (m_breakMin > m_breakMax) m_breakMin = m_breakMax;

        RefreshSensitivity(env);

        // Only help while actually fighting. The holding check goes
        // through Focus so a click in another window while alt-tabbed
        // does not keep the module live.
        if (m_requireSwing) {
            bool swinging = CombatState::TicksSinceSwing() <= m_swingWindow;
            bool holding  = Focus::KeyHeld(VK_LBUTTON);
            if (!swinging && !holding) {
                Rotation::ResetVelocity();
                m_targetId = -1;
                return;
            }
        }

        // Deliberate lapses.
        if (m_breakLeft > 0) { m_breakLeft--; return; }
        if (m_breaks && Roll(m_breakChance)) {
            m_breakLeft = Rand(m_breakMin, m_breakMax);
            return;
        }

        if (!EntityList::Init(env)) return;
        auto ents = EntityList::GetPlayers(env, m_range + 1.0f);
        if (ents.empty()) { m_targetId = -1; m_targetName.clear(); return; }

        float yaw   = Minecraft::GetYaw(env, player);
        float pitch = Minecraft::GetPitch(env, player);

        // ---- Pick a target ----
        EntityInfo* chosen = nullptr;

        if (m_sticky && m_targetId >= 0) {
            for (auto& e : ents) {
                if (EntityId(env, e.ref, e.name) != m_targetId) continue;
                if (e.distanceToPlayer > m_range) break;
                float a = AngleTo(env, player, e.posX, e.posY + m_aimHeight,
                                  e.posZ, yaw, pitch);
                if (a <= m_stickyFov * 0.5f) chosen = &e;
                break;
            }
        }

        if (!chosen) {
            float bestScore = 1e9f;
            for (auto& e : ents) {
                if (e.distanceToPlayer > m_range) continue;

                float a = AngleTo(env, player, e.posX, e.posY + m_aimHeight,
                                  e.posZ, yaw, pitch);
                if (a > m_fov * 0.5f) continue;

                float score;
                switch (m_targetMode) {
                    case 1:  score = (float)e.distanceToPlayer; break;
                    case 2:  score = e.health; break;
                    default: score = a; break;
                }
                if (score < bestScore) { bestScore = score; chosen = &e; }
            }
            m_ticksOnTarget = 0;
        }

        if (!chosen) {
            m_targetId = -1;
            m_targetName.clear();
            Rotation::ResetVelocity();
            return;
        }

        int id = EntityId(env, chosen->ref, chosen->name);
        if (id != m_targetId) { m_targetId = id; m_ticksOnTarget = 0; }
        m_targetName = chosen->name;
        m_ticksOnTarget++;

        // ---- Aim point ----
        if (++m_wanderTick >= kWanderPeriod) {
            m_wanderTick = 0;
            m_wanderX = Randf(-m_wander, m_wander);
            m_wanderY = Randf(-m_wander * 0.7f, m_wander * 0.7f);
            m_wanderZ = Randf(-m_wander, m_wander);
        }

        double tx = chosen->posX + m_wanderX;
        double ty = chosen->posY + m_aimHeight + m_wanderY;
        double tz = chosen->posZ + m_wanderZ;

        auto want = Rotation::ToPoint(env, player, tx, ty, tz);
        m_lastAngle = AngleTo(env, player, tx, ty, tz, yaw, pitch);

        // Pitch ratio and the travel mode both go INTO Step so the
        // grid snap and the velocity carry act on the value that is
        // written. Scaling next.pitch here afterwards was the grid
        // violation this module used to have.
        auto next = Rotation::Step(yaw, pitch, want.yaw, want.pitch,
                                   m_speed, m_smoothing,
                                   m_jitter, m_overshoot,
                                   m_pitchRatio, RotMode());

        Minecraft::SetYaw(env, player, next.yaw);
        Minecraft::SetPitch(env, player, next.pitch);

        m_active = true;
    }

    const char* StatusLine() const override {
        if (m_active && !m_targetName.empty()) {
            snprintf(m_status, sizeof(m_status), "tracking %s  (%.1f\xC2\xB0)",
                     m_targetName.c_str(), m_lastAngle);
        } else {
            snprintf(m_status, sizeof(m_status), "idle");
        }
        return m_status;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!Rotation::HaveSensitivity()) {
            *text = "Your mouse sensitivity could not be read, so a default "
                    "grid is used. Rotations will not line up exactly with "
                    "what your hand can produce.";
            return NoticeLevel::Warning;
        }
        if (m_rotMode == 2) {
            *text = "Snap turns the whole way to the target in a single tick. "
                    "Instant and unmistakable on any prediction anticheat. "
                    "Unprotected servers only.";
            return NoticeLevel::Danger;
        }
        if (m_rotMode == 1) {
            *text = "Linear pulls at a constant speed with no easing, which "
                    "arrives faster but reads less like a hand than Smooth.";
            return NoticeLevel::Warning;
        }
        if (!m_requireSwing) {
            *text = "Only While Swinging is off, so it will track people "
                    "across a lobby while you stand still. That is the "
                    "loudest thing this module can do.";
            return NoticeLevel::Warning;
        }
        if (m_speed > kVisibleSpeed) {
            *text = "Above 6 the correction is visible when someone watches "
                    "the fight back.";
            return NoticeLevel::Warning;
        }
        if (m_wander < 0.05f) {
            *text = "Near-zero wander means the exact centre of the hitbox on "
                    "every hit, which no hand produces.";
            return NoticeLevel::Warning;
        }
        snprintf(m_notice, sizeof(m_notice),
                 "Every rotation is a multiple of your mouse grid, %.4f\xC2\xB0 "
                 "at sensitivity %.2f.",
                 Rotation::GCD(), Rotation::Sensitivity());
        *text = m_notice;
        return NoticeLevel::Info;
    }
};
