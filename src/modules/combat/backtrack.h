#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <unordered_map>
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include <random>
#include <cmath>

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
// their path. Intersect mode instead scans the whole history and
// takes the sample closest to your crosshair that is still inside
// reach. That is where the extra strength comes from, not from a
// longer delay.
//
// STAYING QUIET
//   - per-target delay drawn from a band, re-rolled periodically
//   - pulse mode so the desync appears in bursts, not constantly
//   - range window, because rewinding a distant player buys nothing
//   - ping-aware cap keeping ping + delay inside the comp window
//   - restores on damage, on flag, on GUI, on disable
//   - optional sprint-reset sync: only rewind on the ticks you are
//     actually swinging, which is when it matters
// =================================================================

class Backtrack : public Module {
public:
    // Published for the render thread. Plain data, no JNI.
    struct VisTarget {
        double trueX, trueY, trueZ;      // where the server has them
        double backX, backY, backZ;      // where we are holding them
        std::vector<std::array<double, 3>> trail;
        float  health = 20.f, maxHealth = 20.f;
        int    delayMs = 0;
        double offset = 0.0;             // metres of rewind
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

    // ---- Core ----
    int   m_mode       = 1;      // 0 Constant, 1 Pulse, 2 Adaptive
    int   m_targeting  = 1;      // 0 Oldest, 1 Intersect, 2 Nearest
    int   m_delayMinMs = 60;
    int   m_delayMaxMs = 120;
    int   m_hardCapMs  = 180;

    // ---- Strength ----
    bool  m_aimLock      = false;  // pull the crosshair to the held spot
    float m_aimLockSpeed = 3.0f;
    float m_aimLockFov   = 60.0f;
    bool  m_swingSync    = true;   // only rewind on swing ticks
    int   m_swingWindow  = 3;      // ticks either side of a click
    bool  m_holdThrough  = true;   // keep the pose while a swing lands
    int   m_holdTicks    = 2;
    float m_reachAssist  = 3.0f;   // treat this as your usable reach

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
    bool  m_restoreOnDamage   = true;
    bool  m_onlyWhileClicking = true;
    int   m_pauseAfterFlagTicks = 40;
    int   m_rampTicks = 4;
    float m_perTargetJitter = 24.0f;

    // ---- Visibility ----
    bool  m_visEnabled   = true;
    int   m_visStyle     = 0;    // 0 Box, 1 Wireframe, 2 Trail, 3 Marker, 4 Text
    bool  m_visLink      = true; // line from true position to held one
    bool  m_visShowMs    = true;
    bool  m_visShowDist  = true;
    bool  m_visGhost     = true; // faint marker at the true position
    float m_visThickness = 1.6f;
    float m_visColor[4]  = { 0.45f, 0.85f, 1.00f, 0.95f };
    float m_visGhostA    = 0.30f;

    // ---- State ----
    std::unordered_map<int, Target> m_targets;
    bool m_rewinding    = false;
    int  m_pulseCounter = 0;
    int  m_pulseTarget  = 0;
    int  m_pauseCounter = 0;
    int  m_rampCounter  = 0;
    int  m_lastHurtTime = 0;
    int  m_activeCount  = 0;
    int  m_measuredPing = 30;
    int  m_swingTicksLeft = 0;
    bool m_lastLMB = false;
    unsigned long long m_tick = 0;
    double m_maxOffsetSeen = 0.0;

    // Snapshot handed to the render thread
    std::vector<VisTarget> m_vis;
    std::mutex m_visMutex;

    // ---- JNI ----
    jmethodID m_getEntityId = nullptr;
    jfieldID  m_fPosX = nullptr, m_fPosY = nullptr, m_fPosZ = nullptr;
    jfieldID  m_fPrevX = nullptr, m_fPrevY = nullptr, m_fPrevZ = nullptr;
    bool m_resolved = false;

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
        if (t.ref) { env->DeleteGlobalRef(t.ref); t.ref = nullptr; }
    }

    // Angle between the crosshair and a world point, in degrees
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

        if (m_targeting == 0) {
            // Oldest inside the delay: the classic behaviour
            for (auto it = t.history.rbegin(); it != t.history.rend(); ++it) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->at).count();
                if (age >= t.delayMs) return &(*it);
            }
            return nullptr;
        }

        // Intersect and Nearest both walk the whole history and score
        // every sample. Only samples inside the compensation budget
        // are eligible, otherwise the server rejects the hit.
        const Sample* best = nullptr;
        double bestScore = 1e18;

        for (const auto& s : t.history) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s.at).count();
            if (age > cap) continue;               // outside the window
            if (age < 10) continue;                // effectively now

            double dx = s.x - pX, dy = s.y - pY, dz = s.z - pZ;
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist > m_reachAssist) continue;    // could not hit it anyway

            double score;
            if (m_targeting == 1) {
                // Intersect: closest to where you are already aiming.
                // This is what makes a strafing enemy hittable.
                score = AngleTo(env, player, s.x, s.y, s.z, yaw, pitch);
            } else {
                // Nearest: shortest distance, easiest to land
                score = dist;
            }

            if (score < bestScore) { bestScore = score; best = &s; }
        }

        if (best) return best;

        // Nothing scored: fall back to the plain delay pick so the
        // module still does something rather than silently idling.
        for (auto it = t.history.rbegin(); it != t.history.rend(); ++it) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->at).count();
            if (age >= t.delayMs) return &(*it);
        }
        return nullptr;
    }

public:
    Backtrack() : Module("Backtrack", "Hit players at their past positions",
                         ModuleCategory::COMBAT, 0)
    {
        Bind("Mode", &m_mode);
        Bind("Targeting", &m_targeting);
        Bind("Delay Min", &m_delayMinMs);
        Bind("Delay Max", &m_delayMaxMs);
        Bind("Hard Cap", &m_hardCapMs);
        Bind("Aim Lock", &m_aimLock);
        Bind("Aim Lock Speed", &m_aimLockSpeed);
        Bind("Aim Lock FOV", &m_aimLockFov);
        Bind("Swing Sync", &m_swingSync);
        Bind("Swing Window", &m_swingWindow);
        Bind("Hold Through", &m_holdThrough);
        Bind("Hold Ticks", &m_holdTicks);
        Bind("Reach Assist", &m_reachAssist);
        Bind("Use Range Window", &m_useRangeWindow);
        Bind("Range Min", &m_rangeMin);
        Bind("Range Max", &m_rangeMax);
        Bind("Pulse On Min", &m_pulseOnMin);
        Bind("Pulse On Max", &m_pulseOnMax);
        Bind("Pulse Off Min", &m_pulseOffMin);
        Bind("Pulse Off Max", &m_pulseOffMax);
        Bind("Ping Aware", &m_pingAware);
        Bind("Compensation Window", &m_compensationMs);
        Bind("Safety Margin", &m_safetyMarginMs);
        Bind("Restore On Damage", &m_restoreOnDamage);
        Bind("Only While Clicking", &m_onlyWhileClicking);
        Bind("Pause After Flag", &m_pauseAfterFlagTicks);
        Bind("Ramp Ticks", &m_rampTicks);
        Bind("Per Target Jitter", &m_perTargetJitter);
        Bind("Visibility", &m_visEnabled);
        Bind("Vis Style", &m_visStyle);
        Bind("Vis Link", &m_visLink);
        Bind("Vis Ghost", &m_visGhost);
        Bind("Vis Thickness", &m_visThickness);
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
        m_maxOffsetSeen = 0.0;
    }

    void OnDisable(JNIEnv* env) override {
        RestoreBeforeScan(env);
        for (auto& kv : m_targets) DropTarget(env, kv.second);
        m_targets.clear();
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

        m_tick++;
        if (m_rampCounter < m_rampTicks) m_rampCounter++;

        // ---- Frozen after a correction ----
        if (m_pauseCounter > 0) {
            if (--m_pauseCounter == 0) m_rampCounter = 0;
            PublishVis();
            return;
        }

        // ---- Taking a hit: drop the desync immediately ----
        if (m_restoreOnDamage) {
            int hurt = Minecraft::GetHurtTime(env, player);
            bool justHit = (hurt > 0 && m_lastHurtTime == 0);
            m_lastHurtTime = hurt;
            if (justHit) { PublishVis(); return; }
        }

        // ---- Swing tracking ----
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool justClicked = lmb && !m_lastLMB;
        m_lastLMB = lmb;
        if (justClicked) m_swingTicksLeft = m_swingWindow;
        else if (m_swingTicksLeft > 0) m_swingTicksLeft--;

        // ---- Pulse cycling ----
        if (m_mode == 1) {
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

        bool clicking = lmb;
        bool active = m_rewinding
                   && (!m_onlyWhileClicking || clicking)
                   && (!m_swingSync || m_swingTicksLeft > 0 || m_holdThrough);

        auto now = std::chrono::steady_clock::now();

        // These positions are server truth: RestoreBeforeScan ran
        // before the scan that filled this list.
        auto ents = EntityList::GetPlayers(env, m_rangeMax + 8.0f);

        double pX = Minecraft::GetPosX(env, player);
        double pY = Minecraft::GetPosY(env, player);
        double pZ = Minecraft::GetPosZ(env, player);
        float  yaw   = Minecraft::GetYaw(env, player);
        float  pitch = Minecraft::GetPitch(env, player);

        // Best rewound spot this tick, used by aim lock
        const double* lockTo = nullptr;
        double lockBuf[3] = { 0, 0, 0 };
        float  lockAngle = m_aimLockFov * 0.5f;

        for (auto& e : ents) {
            jint id = env->CallIntMethod(e.ref, m_getEntityId);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

            Target& t = m_targets[(int)id];
            t.lastSeen = m_tick;

            // Global ref so the restore pass can reach this entity
            // next tick, once the local frame is gone.
            if (!t.ref) t.ref = env->NewGlobalRef(e.ref);

            t.trueX = e.posX; t.trueY = e.posY; t.trueZ = e.posZ;
            t.health = e.health; t.maxHealth = e.maxHealth;
            if (t.name.empty()) t.name = e.name;

            t.history.push_back({ e.posX, e.posY, e.posZ, now });

            // A second of history bounds the memory and is far more
            // than any comp window will accept.
            while (t.history.size() > 40) t.history.pop_front();
            while (!t.history.empty()) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - t.history.front().at).count();
                if (age <= 1000) break;
                t.history.pop_front();
            }

            if (!active) continue;

            double dx = e.posX - pX, dy = e.posY - pY, dz = e.posZ - pZ;
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (m_useRangeWindow && (dist < m_rangeMin || dist > m_rangeMax))
                continue;

            if (m_mode == 2) {
                // Adaptive: further targets get more of the budget
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
                    lockTo = lockBuf;
                }
            }
        }

        // ---- Aim lock ----
        // Rewinding puts the hitbox somewhere your crosshair is not.
        // This nudges it back onto the held pose. It is a rotation
        // change like any aim assist, so it carries the same risk.
        if (m_aimLock && lockTo) {
            auto rot = Minecraft::GetRotationsToPos(env, player,
                lockTo[0], lockTo[1] + 1.0, lockTo[2]);
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
            if (m_tick - it->second.lastSeen > 40) {
                DropTarget(env, it->second);
                it = m_targets.erase(it);
            } else {
                ++it;
            }
        }

        PublishVis();
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

    // Render thread reads this. Never touches JNI.
    std::vector<VisTarget> TakeVis() {
        std::lock_guard<std::mutex> lock(m_visMutex);
        return m_vis;
    }

    bool  VisEnabled() const   { return m_visEnabled && IsEnabled(); }
    int   VisStyle() const     { return m_visStyle; }
    bool  VisLink() const      { return m_visLink; }
    bool  VisGhost() const     { return m_visGhost; }
    bool  VisShowMs() const    { return m_visShowMs; }
    bool  VisShowDist() const  { return m_visShowDist; }
    float VisThickness() const { return m_visThickness; }
    float VisGhostAlpha() const{ return m_visGhostA; }
    const float* VisColor() const { return m_visColor; }

    void RenderSettings() override {
        if (!Ready()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Entity fields unresolved: join a world first");
            ImGui::Separator();
        }

        const char* modes[] = { "Constant", "Pulse", "Adaptive" };
        ImGui::Combo("Mode", &m_mode, modes, 3);
        if (m_mode == 0) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "! A constant delay is the easiest pattern to fingerprint");
        }

        const char* targeting[] = { "Oldest", "Intersect", "Nearest" };
        ImGui::Combo("Targeting", &m_targeting, targeting, 3);
        switch (m_targeting) {
            case 0: ImGui::TextDisabled("Holds the oldest sample inside the delay."); break;
            case 1: ImGui::TextDisabled("Picks the past spot closest to your crosshair."); break;
            case 2: ImGui::TextDisabled("Picks the closest past spot. Easiest to land."); break;
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Strength");
        ImGui::SliderFloat("Reach Assist", &m_reachAssist, 2.5f, 4.5f, "%.2f");
        ImGui::TextDisabled("Samples further than this are never chosen.");
        ImGui::Checkbox("Swing Sync", &m_swingSync);
        if (m_swingSync) {
            ImGui::SliderInt("Swing Window", &m_swingWindow, 1, 8);
            ImGui::Checkbox("Hold Through Swing", &m_holdThrough);
            if (m_holdThrough) ImGui::SliderInt("Hold Ticks", &m_holdTicks, 1, 6);
        }
        ImGui::Checkbox("Aim Lock", &m_aimLock);
        if (m_aimLock) {
            ImGui::SliderFloat("Lock Speed", &m_aimLockSpeed, 0.5f, 10.f, "%.1f");
            ImGui::SliderFloat("Lock FOV", &m_aimLockFov, 10.f, 180.f, "%.0f");
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "! Aim lock is a rotation change and carries aim-assist risk");
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Delay band");
        ImGui::SliderInt("Delay Min (ms)", &m_delayMinMs, 10, 250);
        ImGui::SliderInt("Delay Max (ms)", &m_delayMaxMs, 10, 300);
        if (m_delayMinMs > m_delayMaxMs) m_delayMinMs = m_delayMaxMs;
        ImGui::SliderInt("Hard Cap (ms)", &m_hardCapMs, 40, 400);
        ImGui::SliderFloat("Per-Target Jitter", &m_perTargetJitter, 0.f, 50.f, "%.0f%%");
        ImGui::SliderInt("Ramp Ticks", &m_rampTicks, 0, 12);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Range window");
        ImGui::Checkbox("Use Range Window", &m_useRangeWindow);
        if (m_useRangeWindow) {
            ImGui::SliderFloat("Range Min", &m_rangeMin, 1.f, 4.f, "%.1f");
            ImGui::SliderFloat("Range Max", &m_rangeMax, 3.f, 8.f, "%.1f");
            if (m_rangeMin > m_rangeMax) m_rangeMin = m_rangeMax;
        }

        if (m_mode == 1) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.6f, 1.f, 0.7f, 1.f), "Pulse timing");
            ImGui::SliderInt("Rewind Ticks Min", &m_pulseOnMin, 2, 40);
            ImGui::SliderInt("Rewind Ticks Max", &m_pulseOnMax, 2, 50);
            if (m_pulseOnMin > m_pulseOnMax) m_pulseOnMin = m_pulseOnMax;
            ImGui::SliderInt("Clean Ticks Min", &m_pulseOffMin, 2, 40);
            ImGui::SliderInt("Clean Ticks Max", &m_pulseOffMax, 2, 60);
            if (m_pulseOffMin > m_pulseOffMax) m_pulseOffMin = m_pulseOffMax;
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Safety");
        ImGui::Checkbox("Ping Aware", &m_pingAware);
        if (m_pingAware) {
            ImGui::SliderInt("Comp Window (ms)", &m_compensationMs, 100, 400);
            ImGui::SliderInt("Safety Margin (ms)", &m_safetyMarginMs, 0, 120);
            ImGui::Text("Effective cap: %d ms (ping %d)", EffectiveCap(), m_measuredPing);
        }
        ImGui::Checkbox("Restore On Damage", &m_restoreOnDamage);
        ImGui::Checkbox("Only While Clicking", &m_onlyWhileClicking);
        ImGui::SliderInt("Pause After Flag", &m_pauseAfterFlagTicks, 0, 100);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.f, 1.f), "Visibility");
        ImGui::Checkbox("Show Rewind", &m_visEnabled);
        if (m_visEnabled) {
            const char* styles[] = { "Box", "Wireframe", "Trail", "Marker", "Text Only" };
            ImGui::Combo("Style", &m_visStyle, styles, 5);
            switch (m_visStyle) {
                case 0: ImGui::TextDisabled("Filled box at the held position."); break;
                case 1: ImGui::TextDisabled("Hitbox outline, all twelve edges."); break;
                case 2: ImGui::TextDisabled("Path from the held spot to the real one."); break;
                case 3: ImGui::TextDisabled("Small cross at the held position."); break;
                case 4: ImGui::TextDisabled("Just the numbers, no geometry."); break;
            }
            ImGui::Checkbox("Link Line", &m_visLink);
            ImGui::Checkbox("Ghost At True Position", &m_visGhost);
            if (m_visGhost)
                ImGui::SliderFloat("Ghost Alpha", &m_visGhostA, 0.05f, 0.8f, "%.2f");
            ImGui::Checkbox("Show Delay (ms)", &m_visShowMs);
            ImGui::Checkbox("Show Offset (m)", &m_visShowDist);
            ImGui::SliderFloat("Thickness", &m_visThickness, 1.f, 4.f, "%.1f");
            ImGui::ColorEdit4("Color", m_visColor);
        }

        ImGui::Separator();
        ImGui::TextDisabled("Rewound: %d | tracked: %d | %s",
            m_activeCount, (int)m_targets.size(),
            m_rewinding ? "active" : "clean");
        ImGui::TextDisabled("Peak offset this session: %.2f m", m_maxOffsetSeen);
        if (ImGui::SmallButton("Reset peak")) m_maxOffsetSeen = 0.0;

        ImGui::TextWrapped(
            "Rewinds only between your ticks, since there is no packet hook "
            "holding updates at the source. On a low-ping duel server there "
            "is little natural jitter to hide inside, so keep the band short.");
    }
};
