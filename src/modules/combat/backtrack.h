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
#include <chrono>
#include <random>
#include <cmath>

// =================================================================
// Backtrack
// =================================================================
// Makes enemies sit at a position they occupied a short while ago.
// You aim where they WERE; the server's lag compensation still
// counts the hit. Your reach value is never touched.
//
// HOW IT WORKS
// Minecraft raytraces attacks against the entity's posX/Y/Z fields.
// Write an older position into those fields and the swing lands
// there. No packet hook required.
//
// ORDER OF OPERATIONS (this is the whole trick)
//   1. RestoreBeforeScan runs first, before anything reads entities.
//      Every rewound entity goes back to its true position.
//   2. EntityList scans, so every other module and the ESP see the
//      real world rather than our fiction.
//   3. OnTick records the true positions and rewinds again.
//
// An earlier version skipped step 1 and tried to guess whether a
// position packet had landed by comparing the live field to the
// cached one. Both came from the same scan, so the comparison was
// always equal and the true position never updated: targets froze
// in place. Restoring first removes the guesswork entirely.
//
// Targets are held as GLOBAL refs. Local refs die with the tick's
// frame, and the restore has to run before the new frame's scan.
//
// WHY NAIVE BACKTRACK GETS CAUGHT
//   1. A constant delay gives a hit-distance distribution with zero
//      variance. Real lag is noisy.
//   2. Delay that only appears mid-fight. Nobody lags exclusively
//      while holding left click.
//   3. Delay that survives a lagback, compounding the desync.
//   4. Hits landing outside the server's compensation window.
// =================================================================

class Backtrack : public Module {
private:
    struct Sample {
        double x, y, z;
        std::chrono::steady_clock::time_point at;
    };

    struct Target {
        jobject ref = nullptr;        // global, owned by us
        std::deque<Sample> history;
        bool   rewound = false;
        double trueX = 0, trueY = 0, trueZ = 0;
        int    delayMs = 0;
        unsigned long long lastSeen = 0;
    };

    // ---- Settings ----
    int   m_mode       = 1;    // 0 Constant, 1 Pulse, 2 Adaptive
    int   m_delayMinMs = 40;
    int   m_delayMaxMs = 90;
    int   m_hardCapMs  = 140;

    bool  m_useRangeWindow = true;
    float m_rangeMin = 2.4f;
    float m_rangeMax = 4.6f;

    int   m_pulseOnMin  = 6;
    int   m_pulseOnMax  = 14;
    int   m_pulseOffMin = 8;
    int   m_pulseOffMax = 20;

    bool  m_pingAware      = true;
    int   m_compensationMs = 200;
    int   m_safetyMarginMs = 40;

    bool  m_restoreOnDamage   = true;
    bool  m_onlyWhileClicking = true;
    int   m_pauseAfterFlagTicks = 40;
    int   m_rampTicks = 4;
    float m_perTargetJitter = 22.0f;

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
    unsigned long long m_tick = 0;

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
        // Without this the renderer lerps from the real spot to the
        // rewound one and the target smears across the screen.
        if (m_fPrevX) env->SetDoubleField(ent, m_fPrevX, x);
        if (m_fPrevY) env->SetDoubleField(ent, m_fPrevY, y);
        if (m_fPrevZ) env->SetDoubleField(ent, m_fPrevZ, z);
    }

    void DropTarget(JNIEnv* env, Target& t) {
        if (t.ref) { env->DeleteGlobalRef(t.ref); t.ref = nullptr; }
    }

public:
    Backtrack() : Module("Backtrack", "Hit players at their past positions",
                         ModuleCategory::COMBAT, 0)
    {
        Bind("Mode", &m_mode);
        Bind("Delay Min", &m_delayMinMs);
        Bind("Delay Max", &m_delayMaxMs);
        Bind("Hard Cap", &m_hardCapMs);
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
    }

    // -------------------------------------------------------------
    // Step 1. ModuleManager calls this at the very top of the tick,
    // before EntityList scans. Puts every entity back where the
    // server says it is, so nothing else in the client ever sees a
    // rewound position.
    //
    // Safe to call when disabled: that is exactly how a freshly
    // toggled-off module cleans up.
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
    }

    void OnDisable(JNIEnv* env) override {
        RestoreBeforeScan(env);
        for (auto& kv : m_targets) DropTarget(env, kv.second);
        m_targets.clear();
    }

    // Call from the S08 position-reset handler once one exists
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
        if (Minecraft::IsInGui(env)) return;   // already restored above

        m_tick++;
        if (m_rampCounter < m_rampTicks) m_rampCounter++;

        // ---- Frozen after a server correction ----
        if (m_pauseCounter > 0) {
            if (--m_pauseCounter == 0) m_rampCounter = 0;
            return;
        }

        // ---- Taking a hit means the desync has to go now ----
        if (m_restoreOnDamage) {
            int hurt = Minecraft::GetHurtTime(env, player);
            bool justHit = (hurt > 0 && m_lastHurtTime == 0);
            m_lastHurtTime = hurt;
            if (justHit) return;
        }

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

        bool clicking = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool active = m_rewinding && (!m_onlyWhileClicking || clicking);

        auto now = std::chrono::steady_clock::now();

        // Positions read here are server truth: RestoreBeforeScan ran
        // ahead of the scan that filled this list.
        auto ents = EntityList::GetPlayers(env, m_rangeMax + 8.0f);

        double pX = Minecraft::GetPosX(env, player);
        double pY = Minecraft::GetPosY(env, player);
        double pZ = Minecraft::GetPosZ(env, player);

        for (auto& e : ents) {
            jint id = env->CallIntMethod(e.ref, m_getEntityId);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

            Target& t = m_targets[(int)id];
            t.lastSeen = m_tick;

            // Promote to a global ref so the restore pass can reach
            // this entity next tick, after the local frame is gone.
            if (!t.ref) t.ref = env->NewGlobalRef(e.ref);

            t.trueX = e.posX; t.trueY = e.posY; t.trueZ = e.posZ;
            t.history.push_back({ e.posX, e.posY, e.posZ, now });

            // A second of history is plenty and bounds the memory
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
                // Re-roll now and then so the delay is not a constant
                t.delayMs = RollDelay();
            }

            if (t.delayMs <= 0) continue;

            // Newest sample that is at least delayMs old
            const Sample* chosen = nullptr;
            for (auto it = t.history.rbegin(); it != t.history.rend(); ++it) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->at).count();
                if (age >= t.delayMs) { chosen = &(*it); break; }
            }
            if (!chosen) continue;

            WritePos(env, e.ref, chosen->x, chosen->y, chosen->z);
            t.rewound = true;
            m_activeCount++;
        }

        // Forget players who left, releasing their global refs
        for (auto it = m_targets.begin(); it != m_targets.end(); ) {
            if (m_tick - it->second.lastSeen > 40) {
                DropTarget(env, it->second);
                it = m_targets.erase(it);
            } else {
                ++it;
            }
        }
    }

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

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Delay band");
        ImGui::SliderInt("Delay Min (ms)", &m_delayMinMs, 10, 200);
        ImGui::SliderInt("Delay Max (ms)", &m_delayMaxMs, 10, 250);
        if (m_delayMinMs > m_delayMaxMs) m_delayMinMs = m_delayMaxMs;
        ImGui::SliderInt("Hard Cap (ms)", &m_hardCapMs, 40, 300);
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
            ImGui::SliderInt("Rewind Ticks Min", &m_pulseOnMin, 2, 30);
            ImGui::SliderInt("Rewind Ticks Max", &m_pulseOnMax, 2, 40);
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
        ImGui::TextDisabled("Rewound: %d | tracked: %d | %s",
            m_activeCount, (int)m_targets.size(),
            m_rewinding ? "active" : "clean");
        ImGui::TextWrapped(
            "Rewinds only between your ticks. On a low-ping duel server "
            "there is little natural jitter to hide inside, so keep the "
            "band short.");
    }
};
