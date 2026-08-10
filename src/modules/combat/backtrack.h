#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <unordered_map>
#include <deque>
#include <vector>
#include <array>
#include <string>
#include <mutex>
#include <chrono>
#include <random>
#include <cmath>
#include <cstdio>

// =================================================================
// Backtrack
// =================================================================
// Holds enemies at a position they occupied a moment ago. You aim
// where they WERE and the server's lag compensation still counts
// the hit, so your reach value is never touched.
//
// Minecraft raytraces attacks against the entity's posX/Y/Z fields,
// so writing an older position into them is enough. No packet hook.
//
// TICK ORDER (load-bearing)
//   1. RestoreBeforeScan puts every entity back at server truth
//   2. EntityList scans, so the rest of the client sees reality
//   3. OnTick records truth and rewinds again
//
// TARGETING
// Picking "the oldest sample inside the window" wastes most of the
// budget: an enemy strafing past you is only reachable for part of
// their path. Intersect mode scans the whole history and takes the
// sample closest to your crosshair that is still inside reach. That
// is where the strength comes from, not from a longer delay.
//
// IDENTITY
// Targets are keyed by entity ID, and entity IDs are recycled. The
// server hands the same number to a different player after a
// respawn or a lobby change, and the ID alone will happily match a
// stale global ref to a brand new entity. Every lookup therefore
// checks that the ref we kept is still the same object, and starts
// fresh when it is not. Without that check the restore pass writes
// one player's position into another.
//
// THE OVERLAY HAS NO COLOUR PICKER
// It used to carry its own RGBA, which meant the client had one
// accent colour and this module had another, and changing the
// accent left the rewind markers looking like a different program.
// It draws in the client accent now. One fewer setting, and the
// screen finally looks like one product.
//
// STAYING QUIET
//   - per-target delay drawn from a band, re-rolled periodically
//   - pulse mode so the desync appears in bursts, not constantly
//   - range window, because rewinding a distant player buys nothing
//   - ping-aware cap keeping ping + delay inside the comp window
//   - restores on damage, on flag, on GUI, on disable
//   - swing sync: only rewind around the ticks you are actually
//     swinging, taken from CombatState rather than the mouse button
// =================================================================

class Backtrack : public Module {
public:
    // Published for the render thread. Plain data, no JNI.
    struct VisTarget {
        double trueX = 0, trueY = 0, trueZ = 0;   // where the server has them
        double backX = 0, backY = 0, backZ = 0;   // where we are holding them
        std::vector<std::array<double, 3>> trail;
        float  health = 20.f, maxHealth = 20.f;
        int    delayMs = 0;
        double offset = 0.0;                      // metres of rewind
        bool   rewound = false;
        std::string name;
    };

private:
    struct Sample {
        double x, y, z;
        std::chrono::steady_clock::time_point at;
    };

    struct Target {
        jobject ref = nullptr;           // global ref, owned here
        std::deque<Sample> history;
        bool   rewound = false;
        double trueX = 0, trueY = 0, trueZ = 0;
        double backX = 0, backY = 0, backZ = 0;
        int    delayMs = 0;
        float  health = 20.f, maxHealth = 20.f;
        std::string name;
        unsigned long long lastSeen = 0;
    };

    enum Mode { CONSTANT = 0, PULSE, ADAPTIVE };
    enum Pick { OLDEST = 0, INTERSECT, NEAREST };

    static constexpr const char* kModes[]     = { "Constant", "Pulse", "Adaptive" };
    static constexpr const char* kTargeting[] = { "Oldest", "Intersect", "Nearest" };
    static constexpr const char* kStyles[]    = { "Box", "Wire", "Trail", "Marker", "Text" };

    // ---- Core ----
    int   m_mode       = PULSE;
    int   m_targeting  = INTERSECT;
    int   m_delayMinMs = 60;
    int   m_delayMaxMs = 120;
    int   m_hardCapMs  = 180;

    // ---- Strength ----
    bool  m_aimLock      = false;
    float m_aimLockSpeed = 3.0f;
    float m_aimLockFov   = 60.0f;
    bool  m_swingSync    = true;
    int   m_swingWindow  = 3;
    bool  m_holdThrough  = true;
    int   m_holdTicks    = 2;
    float m_reachAssist  = 3.0f;

    // ---- Window ----
    bool  m_useRangeWindow = true;
    float m_rangeMin = 2.2f;
    float m_rangeMax = 5.2f;

    // ---- Pulse ----
    int   m_pulseOnMin  = 8;
    int   m_pulseOnMax  = 18;
    int   m_pulseOffMin = 6;
    int   m_pulseOffMax = 16;

    // ---- Safety ----
    bool  m_pingAware      = true;
    int   m_compensationMs = 220;
    int   m_safetyMarginMs = 35;
    bool  m_restoreOnDamage = true;
    bool  m_onlyInCombat    = true;
    int   m_pauseAfterFlagTicks = 40;
    int   m_rampTicks = 4;
    float m_perTargetJitter = 24.0f;

    // ---- Visibility ----
    bool  m_visEnabled   = true;
    int   m_visStyle     = 0;
    bool  m_visLink      = true;
    bool  m_visShowMs    = true;
    bool  m_visShowDist  = true;
    bool  m_visGhost     = true;
    float m_visThickness = 1.6f;
    float m_visGhostA    = 0.30f;

    // ---- State ----
    std::unordered_map<int, Target> m_targets;
    bool m_rewinding    = false;
    int  m_pulseCounter = 0;
    int  m_pulseTarget  = 0;
    int  m_pauseCounter = 0;
    int  m_rampCounter  = 0;
    int  m_activeCount  = 0;
    int  m_measuredPing = 30;
    int  m_swingTicksLeft = 0;
    int  m_holdLeft = 0;
    unsigned long long m_tick = 0;
    double m_maxOffsetSeen = 0.0;

    mutable char m_status[64] = { 0 };
    mutable char m_notice[200] = { 0 };

    // Snapshot handed to the render thread
    std::vector<VisTarget> m_vis;
    std::mutex m_visMutex;

    // ---- JNI ----
    jmethodID m_getEntityId = nullptr;
    jfieldID  m_fPosX = nullptr, m_fPosY = nullptr, m_fPosZ = nullptr;
    jfieldID  m_fPrevX = nullptr, m_fPrevY = nullptr, m_fPrevZ = nullptr;
    bool m_resolved = false;

    // Memory bounds. A second of history at 20 TPS is 20 samples;
    // 40 leaves room for a fast tick loop without ever growing.
    static constexpr size_t kMaxSamples  = 40;
    static constexpr long long kMaxAgeMs = 1000;
    static constexpr unsigned long long kForgetTicks = 40;

    // A sample younger than this is indistinguishable from now and
    // gains nothing, so it is never chosen.
    static constexpr long long kMinUsefulAgeMs = 10;

    std::mt19937 m_rng{ std::random_device{}() };

    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        std::uniform_int_distribution<int> d(lo, hi);
        return d(m_rng);
    }

    static float WrapAngle(float a) {
        while (a > 180.f)  a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    }

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::entity) {
            auto e = ClassResolver::entity;
            m_getEntityId = JvmtiUtil::FindMethod(env, e,
                { "func_145782_y", "getEntityId" }, 0);
            m_fPosX  = JvmtiUtil::FindField(env, e, { "field_70165_t", "posX" });
            m_fPosY  = JvmtiUtil::FindField(env, e, { "field_70163_u", "posY" });
            m_fPosZ  = JvmtiUtil::FindField(env, e, { "field_70161_v", "posZ" });
            m_fPrevX = JvmtiUtil::FindField(env, e, { "field_70169_q", "prevPosX" });
            m_fPrevY = JvmtiUtil::FindField(env, e, { "field_70167_r", "prevPosY" });
            m_fPrevZ = JvmtiUtil::FindField(env, e, { "field_70166_s", "prevPosZ" });
        }
        m_resolved = true;
    }

    bool Ready() const {
        return m_getEntityId && m_fPosX && m_fPosY && m_fPosZ;
    }

    int EffectiveCap() const {
        int cap = m_hardCapMs;
        if (m_pingAware) {
            int budget = m_compensationMs - m_measuredPing - m_safetyMarginMs;
            if (budget < cap) cap = budget;
        }
        return cap < 0 ? 0 : cap;
    }

    int RollDelay() {
        int base = Rand(m_delayMinMs, m_delayMaxMs);
        if (m_perTargetJitter > 0.f) {
            float r = base * (m_perTargetJitter / 100.f);
            std::uniform_real_distribution<float> d(-r, r);
            base += (int)d(m_rng);
        }
        if (m_rampTicks > 0 && m_rampCounter < m_rampTicks)
            base = (int)(base * ((float)m_rampCounter / (float)m_rampTicks));

        int cap = EffectiveCap();
        if (base > cap) base = cap;
        return base < 0 ? 0 : base;
    }

    void WritePos(JNIEnv* env, jobject ent, double x, double y, double z) {
        env->SetDoubleField(ent, m_fPosX, x);
        env->SetDoubleField(ent, m_fPosY, y);
        env->SetDoubleField(ent, m_fPosZ, z);
        // prevPos too, or the renderer lerps between the real spot
        // and the held one and the target smears across the screen.
        if (m_fPrevX) env->SetDoubleField(ent, m_fPrevX, x);
        if (m_fPrevY) env->SetDoubleField(ent, m_fPrevY, y);
        if (m_fPrevZ) env->SetDoubleField(ent, m_fPrevZ, z);
    }

    void DropTarget(JNIEnv* env, Target& t) {
        if (t.ref && env) { env->DeleteGlobalRef(t.ref); }
        t.ref = nullptr;
        t.rewound = false;
        t.history.clear();
    }

    void DropAllTargets(JNIEnv* env) {
        for (auto& kv : m_targets) DropTarget(env, kv.second);
        m_targets.clear();
    }

    float AngleTo(JNIEnv* env, jobject player, double x, double y, double z,
                  float yaw, float pitch)
    {
        auto rot = Minecraft::GetRotationsToPos(env, player, x, y + 1.0, z);
        float dy = WrapAngle(rot.yaw - yaw);
        float dp = rot.pitch - pitch;
        return std::sqrt(dy * dy + dp * dp);
    }

    // Choose which past position to hold the target at.
    const Sample* PickSample(JNIEnv* env, jobject player, Target& t,
                             double pX, double pY, double pZ,
                             float yaw, float pitch,
                             std::chrono::steady_clock::time_point now)
    {
        if (t.history.empty()) return nullptr;

        const int cap = EffectiveCap();

        if (m_targeting == OLDEST) {
            for (auto it = t.history.rbegin(); it != t.history.rend(); ++it) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->at).count();
                if (age >= t.delayMs) return &(*it);
            }
            return nullptr;
        }

        // Intersect and Nearest score every sample in the history.
        // Only samples inside the compensation budget are eligible,
        // otherwise the server rejects the hit outright.
        const Sample* best = nullptr;
        double bestScore = 1e18;

        for (const auto& s : t.history) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s.at).count();
            if (age > cap) continue;
            if (age < kMinUsefulAgeMs) continue;

            double dx = s.x - pX, dy = s.y - pY, dz = s.z - pZ;
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist > m_reachAssist) continue;

            double score = (m_targeting == INTERSECT)
                ? AngleTo(env, player, s.x, s.y, s.z, yaw, pitch)
                : dist;

            if (score < bestScore) { bestScore = score; best = &s; }
        }

        if (best) return best;

        // Nothing scored: fall back to the plain delay pick rather
        // than idling.
        for (auto it = t.history.rbegin(); it != t.history.rend(); ++it) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->at).count();
            if (age >= t.delayMs) return &(*it);
        }
        return nullptr;
    }

    // Plain-data copy for the render thread
    void PublishVis() {
        if (!m_visEnabled) {
            std::lock_guard<std::mutex> lock(m_visMutex);
            m_vis.clear();
            return;
        }

        std::vector<VisTarget> out;
        out.reserve(m_targets.size());

        for (auto& kv : m_targets) {
            Target& t = kv.second;
            if (!t.rewound) continue;

            VisTarget v;
            v.trueX = t.trueX; v.trueY = t.trueY; v.trueZ = t.trueZ;
            v.backX = t.backX; v.backY = t.backY; v.backZ = t.backZ;
            v.health = t.health; v.maxHealth = t.maxHealth;
            v.delayMs = t.delayMs;
            v.rewound = true;
            v.name = t.name;

            double dx = t.backX - t.trueX;
            double dy = t.backY - t.trueY;
            double dz = t.backZ - t.trueZ;
            v.offset = std::sqrt(dx*dx + dy*dy + dz*dz);

            if (m_visStyle == 2) {
                v.trail.reserve(t.history.size());
                for (const auto& s : t.history)
                    v.trail.push_back({ s.x, s.y, s.z });
            }

            out.push_back(std::move(v));
        }

        std::lock_guard<std::mutex> lock(m_visMutex);
        m_vis.swap(out);
    }

public:
    Backtrack() : Module("Backtrack", "Hit players at their past positions",
                         ModuleCategory::COMBAT, 0)
    {
        BindMode("Mode", &m_mode, kModes, 3,
                 "Pulse rewinds in bursts and is clean in between, which is "
                 "much harder to fingerprint than a constant delay.");

        BindMode("Targeting", &m_targeting, kTargeting, 3,
                 "Intersect scans the whole history and takes the past spot "
                 "closest to your crosshair. That is where the strength "
                 "comes from, not from a longer delay.");

        Bind("Delay Min", &m_delayMinMs, 10, 250,
             "Milliseconds of rewind, lower bound");

        Bind("Delay Max", &m_delayMaxMs, 10, 300,
             "Milliseconds of rewind, upper bound");

        Bind("Show Rewind", &m_visEnabled,
             "Draw where each target is actually being held");

        // ---- Strength ----
        Bind("Reach Assist", &m_reachAssist, 2.5f, 4.5f, "%.2f",
             "Samples further away than this are never chosen, so your "
             "apparent reach never changes")
            .Advanced();

        Bind("Swing Sync", &m_swingSync,
             "Only rewind around the ticks you are actually swinging")
            .Advanced();

        Bind("Swing Window", &m_swingWindow, 1, 8,
             "Ticks after a swing that the rewind stays on")
            .When("Swing Sync", 1).Advanced();

        Bind("Hold Through Swing", &m_holdThrough,
             "Keep it a few ticks longer so a swing already in flight lands")
            .When("Swing Sync", 1).Advanced();

        Bind("Hold Ticks", &m_holdTicks, 1, 6)
            .When("Swing Sync", 1).Advanced();

        Bind("Aim Lock", &m_aimLock,
             "Nudge the crosshair onto the held pose. This is a rotation "
             "change and carries the same risk as any aim assist.")
            .Advanced();

        Bind("Lock Speed", &m_aimLockSpeed, 0.5f, 10.0f, "%.1f")
            .When("Aim Lock", 1).Advanced();

        Bind("Lock FOV", &m_aimLockFov, 10.0f, 180.0f, "%.0f")
            .When("Aim Lock", 1).Advanced();

        // ---- Delay band ----
        Bind("Hard Cap", &m_hardCapMs, 40, 400,
             "Absolute ceiling on the rewind, whatever else asks for")
            .Advanced();

        Bind("Per-Target Jitter", &m_perTargetJitter, 0.0f, 50.0f, "%.0f%%",
             "Everyone gets a slightly different delay")
            .Advanced();

        Bind("Ramp Ticks", &m_rampTicks, 0, 12,
             "Ease into the full delay rather than snapping to it")
            .Advanced();

        // ---- Range window ----
        Bind("Use Range Window", &m_useRangeWindow,
             "Rewinding someone across the map buys nothing and is one more "
             "thing to be seen doing")
            .Advanced();

        Bind("Range Min", &m_rangeMin, 1.0f, 4.0f, "%.1f")
            .When("Use Range Window", 1).Advanced();

        Bind("Range Max", &m_rangeMax, 3.0f, 8.0f, "%.1f")
            .When("Use Range Window", 1).Advanced();

        // ---- Pulse timing ----
        Bind("Rewind Ticks Min", &m_pulseOnMin, 2, 40,
             "How long each burst of rewinding lasts")
            .When("Mode", PULSE).Advanced();

        Bind("Rewind Ticks Max", &m_pulseOnMax, 2, 50)
            .When("Mode", PULSE).Advanced();

        Bind("Clean Ticks Min", &m_pulseOffMin, 2, 40,
             "How long it stays completely clean in between")
            .When("Mode", PULSE).Advanced();

        Bind("Clean Ticks Max", &m_pulseOffMax, 2, 60)
            .When("Mode", PULSE).Advanced();

        // ---- Safety ----
        Bind("Ping Aware", &m_pingAware,
             "Keep your ping plus the rewind inside the server's lag "
             "compensation window, or the hits simply do not register")
            .Advanced();

        Bind("Comp Window", &m_compensationMs, 100, 400,
             "How far back the server is willing to look")
            .When("Ping Aware", 1).Advanced();

        Bind("Safety Margin", &m_safetyMarginMs, 0, 120,
             "Headroom left inside that window")
            .When("Ping Aware", 1).Advanced();

        Bind("Restore On Damage", &m_restoreOnDamage,
             "Drop the desync the moment you take a hit")
            .Advanced();

        Bind("Only In Combat", &m_onlyInCombat,
             "Stay completely clean until you are actually fighting")
            .Advanced();

        Bind("Pause After Flag", &m_pauseAfterFlagTicks, 0, 100,
             "Ticks of quiet after the server corrects your position")
            .Advanced();

        // ---- Visibility ----
        BindMode("Style", &m_visStyle, kStyles, 5,
                 "Box is solid, Wire is the hitbox outline, Trail shows the "
                 "path back to where they really are, Marker is a small "
                 "cross and Text is just the numbers.")
            .When("Show Rewind", 1).Advanced();

        Bind("Link Line", &m_visLink,
             "Line from the held position to the real one")
            .When("Show Rewind", 1).Advanced();

        Bind("Ghost At True Position", &m_visGhost,
             "Faint marker where the server actually has them")
            .When("Show Rewind", 1).Advanced();

        Bind("Ghost Alpha", &m_visGhostA, 0.05f, 0.8f, "%.2f")
            .When("Show Rewind", 1).Advanced();

        Bind("Show Delay", &m_visShowMs,
             "Print the rewind in milliseconds")
            .When("Show Rewind", 1).Advanced();

        Bind("Show Offset", &m_visShowDist,
             "Print how far back they are being held, in metres")
            .When("Show Rewind", 1).Advanced();

        Bind("Thickness", &m_visThickness, 1.0f, 4.0f, "%.1f")
            .When("Show Rewind", 1).Advanced();
    }

    // -------------------------------------------------------------
    // Step 1. ModuleManager calls this at the very top of the tick,
    // before EntityList scans. Runs even while disabled, so a module
    // switched off mid-rewind still leaves the world correct.
    // -------------------------------------------------------------
    void RestoreBeforeScan(JNIEnv* env) {
        if (!Ready() || m_targets.empty()) return;
        for (auto& kv : m_targets) {
            Target& t = kv.second;
            if (!t.rewound || !t.ref) continue;
            WritePos(env, t.ref, t.trueX, t.trueY, t.trueZ);
            t.rewound = false;
        }
        m_activeCount = 0;
    }

    void OnEnable(JNIEnv*) override {
        m_rampCounter  = 0;
        m_pulseCounter = 0;
        m_pulseTarget  = Rand(m_pulseOnMin, m_pulseOnMax);
        m_rewinding    = true;
        m_pauseCounter = 0;
        m_activeCount  = 0;
        m_holdLeft     = 0;
        m_swingTicksLeft = 0;
        m_maxOffsetSeen = 0.0;
    }

    void OnDisable(JNIEnv* env) override {
        RestoreBeforeScan(env);
        DropAllTargets(env);
        m_status[0] = '\0';
        std::lock_guard<std::mutex> lock(m_visMutex);
        m_vis.clear();
    }

    // -------------------------------------------------------------
    // World change, respawn, reconnect.
    //
    // Every recorded position belongs to a world that no longer
    // exists, and every global ref points at an entity that is gone.
    // Restore first, in case we were mid-rewind, then forget all of
    // it. Keeping the history across a world change is how a rewind
    // ends up teleporting a freshly spawned player into the last
    // map's geometry.
    // -------------------------------------------------------------
    void OnReset(JNIEnv* env) override {
        RestoreBeforeScan(env);
        DropAllTargets(env);

        m_rewinding      = IsEnabled();
        m_pulseCounter   = 0;
        m_pulseTarget    = Rand(m_pulseOnMin, m_pulseOnMax);
        m_pauseCounter   = 0;
        m_rampCounter    = 0;   // ease back in rather than snapping to full delay
        m_activeCount    = 0;
        m_swingTicksLeft = 0;
        m_holdLeft       = 0;
        m_maxOffsetSeen  = 0.0;

        std::lock_guard<std::mutex> lock(m_visMutex);
        m_vis.clear();
    }

    void OnServerCorrection(JNIEnv* env) {
        RestoreBeforeScan(env);
        m_pauseCounter = m_pauseAfterFlagTicks;
        m_rewinding = false;
    }

    void SetPing(int ping) { m_measuredPing = ping; }

    void OnTick(JNIEnv* env) override {
        Resolve(env);
        if (!Ready()) return;
        if (!EntityList::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;   // restored above already

        // Hand-edited configs can invert any of these pairs, and an
        // inverted band silently disables the module.
        if (m_delayMinMs  > m_delayMaxMs)  m_delayMinMs  = m_delayMaxMs;
        if (m_rangeMin    > m_rangeMax)    m_rangeMin    = m_rangeMax;
        if (m_pulseOnMin  > m_pulseOnMax)  m_pulseOnMin  = m_pulseOnMax;
        if (m_pulseOffMin > m_pulseOffMax) m_pulseOffMin = m_pulseOffMax;

        m_tick++;
        if (m_rampCounter < m_rampTicks) m_rampCounter++;

        // ---- Frozen after a correction ----
        if (m_pauseCounter > 0) {
            if (--m_pauseCounter == 0) m_rampCounter = 0;
            PublishVis();
            return;
        }

        // ---- Taking a hit: drop the desync immediately ----
        if (m_restoreOnDamage && CombatState::HitTakenThisTick()) {
            PublishVis();
            return;
        }

        // ---- Swing tracking ----
        if (CombatState::SwungThisTick()) {
            m_swingTicksLeft = m_swingWindow;
            m_holdLeft = m_holdThrough ? m_holdTicks : 0;
        } else {
            if (m_swingTicksLeft > 0) m_swingTicksLeft--;
            if (m_holdLeft > 0) m_holdLeft--;
        }

        // ---- Pulse cycling ----
        if (m_mode == PULSE) {
            if (++m_pulseCounter >= m_pulseTarget) {
                m_rewinding = !m_rewinding;
                m_pulseCounter = 0;
                m_pulseTarget = m_rewinding ? Rand(m_pulseOnMin, m_pulseOnMax)
                                            : Rand(m_pulseOffMin, m_pulseOffMax);
                if (m_rewinding) m_rampCounter = 0;
            }
        } else {
            m_rewinding = true;
        }

        bool swingOk = !m_swingSync
                    || m_swingTicksLeft > 0
                    || m_holdLeft > 0;

        bool active = m_rewinding
                   && (!m_onlyInCombat || CombatState::InCombat())
                   && swingOk;

        auto now = std::chrono::steady_clock::now();

        // These positions are server truth: RestoreBeforeScan ran
        // before the scan that filled this list.
        auto ents = EntityList::GetPlayers(env, m_rangeMax + 8.0f);

        double pX = Minecraft::GetPosX(env, player);
        double pY = Minecraft::GetPosY(env, player);
        double pZ = Minecraft::GetPosZ(env, player);
        float  yaw   = Minecraft::GetYaw(env, player);
        float  pitch = Minecraft::GetPitch(env, player);

        double lockBuf[3] = { 0, 0, 0 };
        bool   haveLock = false;
        float  lockAngle = m_aimLockFov * 0.5f;

        for (auto& e : ents) {
            jint id = env->CallIntMethod(e.ref, m_getEntityId);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

            Target& t = m_targets[(int)id];

            // IDs are recycled. If the ref we are holding is not this
            // object, the slot belongs to somebody else now and every
            // sample in it is another player's path.
            if (t.ref && !env->IsSameObject(t.ref, e.ref)) {
                DropTarget(env, t);
                t.delayMs = 0;
                t.name.clear();
            }

            t.lastSeen = m_tick;

            // Global ref so the restore pass can reach this entity
            // next tick, once the local frame is gone.
            if (!t.ref) t.ref = env->NewGlobalRef(e.ref);

            t.trueX = e.posX; t.trueY = e.posY; t.trueZ = e.posZ;
            t.health = e.health; t.maxHealth = e.maxHealth;
            if (t.name.empty()) t.name = e.name;

            t.history.push_back({ e.posX, e.posY, e.posZ, now });

            while (t.history.size() > kMaxSamples) t.history.pop_front();
            while (!t.history.empty()) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - t.history.front().at).count();
                if (age <= kMaxAgeMs) break;
                t.history.pop_front();
            }

            if (!active) continue;

            double dx = e.posX - pX, dy = e.posY - pY, dz = e.posZ - pZ;
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (m_useRangeWindow && (dist < m_rangeMin || dist > m_rangeMax))
                continue;

            if (m_mode == ADAPTIVE) {
                float span = m_rangeMax - m_rangeMin;
                float f = span > 0.f ? (float)((dist - m_rangeMin) / span) : 0.5f;
                if (f < 0.f) f = 0.f;
                if (f > 1.f) f = 1.f;
                t.delayMs = m_delayMinMs + (int)((EffectiveCap() - m_delayMinMs) * f);
            } else if (t.delayMs == 0 || (m_tick % 10) == 0) {
                t.delayMs = RollDelay();
            }
            if (t.delayMs <= 0) continue;

            const Sample* chosen = PickSample(env, player, t,
                                              pX, pY, pZ, yaw, pitch, now);
            if (!chosen) continue;

            WritePos(env, e.ref, chosen->x, chosen->y, chosen->z);
            t.backX = chosen->x; t.backY = chosen->y; t.backZ = chosen->z;
            t.rewound = true;
            m_activeCount++;

            double ox = chosen->x - t.trueX;
            double oy = chosen->y - t.trueY;
            double oz = chosen->z - t.trueZ;
            double off = std::sqrt(ox*ox + oy*oy + oz*oz);
            if (off > m_maxOffsetSeen) m_maxOffsetSeen = off;

            if (m_aimLock) {
                float a = AngleTo(env, player, chosen->x, chosen->y, chosen->z,
                                  yaw, pitch);
                if (a < lockAngle) {
                    lockAngle = a;
                    lockBuf[0] = chosen->x;
                    lockBuf[1] = chosen->y;
                    lockBuf[2] = chosen->z;
                    haveLock = true;
                }
            }
        }

        // ---- Aim lock ----
        if (m_aimLock && haveLock) {
            auto rot = Minecraft::GetRotationsToPos(env, player,
                lockBuf[0], lockBuf[1] + 1.0, lockBuf[2]);
            float dy = WrapAngle(rot.yaw - yaw);
            float dp = rot.pitch - pitch;
            float step = m_aimLockSpeed * 0.02f;
            float nYaw   = yaw   + dy * step;
            float nPitch = pitch + dp * step;
            if (nPitch >  90.f) nPitch =  90.f;
            if (nPitch < -90.f) nPitch = -90.f;
            Minecraft::SetYaw(env, player, nYaw);
            Minecraft::SetPitch(env, player, nPitch);
        }

        // ---- Forget players who left ----
        for (auto it = m_targets.begin(); it != m_targets.end(); ) {
            if (m_tick - it->second.lastSeen > kForgetTicks) {
                DropTarget(env, it->second);
                it = m_targets.erase(it);
            } else {
                ++it;
            }
        }

        PublishVis();
    }

    // ---- Read by the render thread. No JNI. ----
    std::vector<VisTarget> TakeVis() {
        std::lock_guard<std::mutex> lock(m_visMutex);
        return m_vis;
    }

    bool  VisEnabled() const    { return m_visEnabled && IsEnabled(); }
    int   VisStyle() const      { return m_visStyle; }
    bool  VisLink() const       { return m_visLink; }
    bool  VisGhost() const      { return m_visGhost; }
    bool  VisShowMs() const     { return m_visShowMs; }
    bool  VisShowDist() const   { return m_visShowDist; }
    float VisThickness() const  { return m_visThickness; }
    float VisGhostAlpha() const { return m_visGhostA; }

    const char* StatusLine() const override {
        if (m_activeCount > 0) {
            snprintf(m_status, sizeof(m_status), "holding %d  \xc2\xb7  %.2f m peak",
                     m_activeCount, m_maxOffsetSeen);
        } else {
            snprintf(m_status, sizeof(m_status), "%s",
                     m_pauseCounter > 0 ? "paused after a flag"
                                        : (m_rewinding ? "armed" : "clean"));
        }
        return m_status;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!Ready()) {
            *text = "The entity position fields have not been found yet. "
                    "Join a world and it will resolve itself.";
            return NoticeLevel::Warning;
        }
        if (m_mode == CONSTANT) {
            *text = "A constant delay is the easiest possible pattern to "
                    "fingerprint. Pulse exists for a reason.";
            return NoticeLevel::Warning;
        }
        if (EffectiveCap() < 20) {
            snprintf(m_notice, sizeof(m_notice),
                     "At %d ping there is almost no budget left inside the "
                     "server's compensation window, so this will do very "
                     "little.", m_measuredPing);
            *text = m_notice;
            return NoticeLevel::Warning;
        }
        snprintf(m_notice, sizeof(m_notice),
                 "Effective cap %d ms at %d ping. Peak rewind this session "
                 "%.2f m.", EffectiveCap(), m_measuredPing, m_maxOffsetSeen);
        *text = m_notice;
        return NoticeLevel::Info;
    }
};
