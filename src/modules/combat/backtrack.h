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
// Makes enemies render and take hits at a position they occupied a
// short while ago. You aim where they WERE; the server's lag
// compensation still counts it. Your reach value is never touched.
//
// HOW THIS VERSION WORKS
//
// The previous design waited on a Netty packet hook that was never
// written, so the module did nothing. This one needs no hook.
//
// Every tick we record each nearby player's real position, then
// write an older recorded position back into the entity. Minecraft
// raytraces attacks against whatever is in those fields, so the
// swing lands on the rewound position. The next position packet
// overwrites our value, and we re-apply on the following tick.
//
// prevPos is rewritten alongside pos, otherwise the renderer
// interpolates between the old and new spots and the target smears
// across the screen.
//
// WHY NAIVE BACKTRACK GETS CAUGHT
//   1. A constant delay gives a hit-distance distribution with zero
//      variance. Real lag is noisy.
//   2. Delay that only appears mid-fight. Nobody lags exclusively
//      while holding left click.
//   3. Delay that survives a lagback, compounding the desync.
//   4. Hits landing outside the server's compensation window.
//
// WHAT THIS DOES ABOUT IT
//   - per-target random delay inside a band, never a constant
//   - pulse mode: rewind in short bursts, run clean between them
//   - range window: only active where backtrack actually helps
//   - ping-aware cap so ping + delay stays inside the window
//   - restores true positions on damage, on flag, and on disable
//   - ramp-in so the delay grows over several ticks
// =================================================================

class Backtrack : public Module {
private:
    struct Sample {
        double x, y, z;
        std::chrono::steady_clock::time_point at;
    };

    struct Target {
        std::deque<Sample> history;
        bool   rewound = false;
        double trueX = 0, trueY = 0, trueZ = 0;
        int    delayMs = 0;
        int    lastSeen = 0;
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
    int  m_tickCounter  = 0;
    int  m_activeCount  = 0;
    int  m_measuredPing = 30;

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
        printf("[Backtrack] id=%p posX=%p prevX=%p\n",
            (void*)m_getEntityId, (void*)m_fPosX, (void*)m_fPrevX);
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
        if (m_rampTicks > 0 && m_rampCounter < m_rampTicks) {
            base = (int)(base * ((float)m_rampCounter / (float)m_rampTicks));
        }

        int cap = EffectiveCap();
        if (base > cap) base = cap;
        return base < 0 ? 0 : base;
    }

    int EntityId(JNIEnv* env, jobject ent) {
        if (!m_getEntityId) return -1;
        jint id = env->CallIntMethod(ent, m_getEntityId);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
        return (int)id;
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

    // Put every rewound entity back where the server thinks it is
    void RestoreAll(JNIEnv* env) {
        if (!Ready()) { m_targets.clear(); return; }

        auto ents = EntityList::GetPlayers(env, 64.0f);
        for (auto& e : ents) {
            int id = EntityId(env, e.ref);
            auto it = m_targets.find(id);
            if (it == m_targets.end() || !it->second.rewound) continue;
            WritePos(env, e.ref, it->second.trueX, it->second.trueY, it->second.trueZ);
            it->second.rewound = false;
        }
        m_activeCount = 0;
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

    void OnEnable(JNIEnv*) override {
        m_targets.clear();
        m_rampCounter  = 0;
        m_pulseCounter = 0;
        m_pulseTarget  = Rand(m_pulseOnMin, m_pulseOnMax);
        m_rewinding    = true;
        m_pauseCounter = 0;
        m_activeCount  = 0;
    }

    void OnDisable(JNIEnv* env) override {
        RestoreAll(env);
        m_targets.clear();
    }

    // Call from the S08 position-reset handler once one exists
    void OnServerCorrection(JNIEnv* env) {
        RestoreAll(env);
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
        if (Minecraft::IsInGui(env)) { RestoreAll(env); return; }

        m_tickCounter++;
        if (m_rampCounter < m_rampTicks) m_rampCounter++;

        // ---- Frozen after a server correction ----
        if (m_pauseCounter > 0) {
            if (--m_pauseCounter == 0) m_rampCounter = 0;
            RestoreAll(env);
            return;
        }

        // ---- Taking a hit means the desync has to go, now ----
        if (m_restoreOnDamage) {
            int hurt = Minecraft::GetHurtTime(env, player);
            bool justHit = (hurt > 0 && m_lastHurtTime == 0);
            m_lastHurtTime = hurt;
            if (justHit) { RestoreAll(env); return; }
        }

        // ---- Pulse cycling ----
        if (m_mode == 1) {
            if (++m_pulseCounter >= m_pulseTarget) {
                m_rewinding = !m_rewinding;
                m_pulseCounter = 0;
                m_pulseTarget = m_rewinding ? Rand(m_pulseOnMin, m_pulseOnMax)
                                            : Rand(m_pulseOffMin, m_pulseOffMax);
                if (!m_rewinding) { RestoreAll(env); return; }
                m_rampCounter = 0;
            }
        } else {
            m_rewinding = true;
        }

        bool clicking = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool active = m_rewinding && (!m_onlyWhileClicking || clicking);

        if (!active) { RestoreAll(env); return; }

        auto now = std::chrono::steady_clock::now();
        auto ents = EntityList::GetPlayers(env, m_rangeMax + 8.0f);
        m_activeCount = 0;

        for (auto& e : ents) {
            int id = EntityId(env, e.ref);
            if (id < 0) continue;

            Target& t = m_targets[id];
            t.lastSeen = m_tickCounter;

            // The live fields hold a rewound value from last tick
            // unless a packet has landed since, so read the snapshot
            // EntityList took rather than the fields themselves.
            double curX = e.posX, curY = e.posY, curZ = e.posZ;
            if (t.rewound) {
                // Nothing new arrived: our own write is still there
                double lx = env->GetDoubleField(e.ref, m_fPosX);
                double lz = env->GetDoubleField(e.ref, m_fPosZ);
                bool untouched =
                    std::fabs(lx - curX) < 1e-6 && std::fabs(lz - curZ) < 1e-6;
                if (untouched) { curX = t.trueX; curY = t.trueY; curZ = t.trueZ; }
            }

            t.trueX = curX; t.trueY = curY; t.trueZ = curZ;
            t.history.push_back({ curX, curY, curZ, now });

            // Keep a second of history, no more
            while (t.history.size() > 40) t.history.pop_front();
            while (!t.history.empty()) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - t.history.front().at).count();
                if (age <= 1000) break;
                t.history.pop_front();
            }

            // Distance is measured against the real position, not
            // the rewound one, or the window would drift.
            double dx = curX - Minecraft::GetPosX(env, player);
            double dy = curY - Minecraft::GetPosY(env, player);
            double dz = curZ - Minecraft::GetPosZ(env, player);
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);

            bool inWindow = !m_useRangeWindow
                         || (dist >= m_rangeMin && dist <= m_rangeMax);

            if (!inWindow) {
                if (t.rewound) {
                    WritePos(env, e.ref, t.trueX, t.trueY, t.trueZ);
                    t.rewound = false;
                }
                continue;
            }

            if (m_mode == 2) {
                // Adaptive: further targets get more of the budget
                float span = m_rangeMax - m_rangeMin;
                float f = span > 0.f ? (float)((dist - m_rangeMin) / span) : 0.5f;
                if (f < 0.f) f = 0.f;
                if (f > 1.f) f = 1.f;
                t.delayMs = m_delayMinMs + (int)((EffectiveCap() - m_delayMinMs) * f);
            } else if (t.delayMs == 0 || (m_tickCounter % 10) == 0) {
                // Re-roll occasionally so the delay is not a constant
                t.delayMs = RollDelay();
            }

            if (t.delayMs <= 0) continue;

            // Pick the newest sample at least delayMs old
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

        // Forget players who left
        for (auto it = m_targets.begin(); it != m_targets.end(); ) {
            if (m_tickCounter - it->second.lastSeen > 40) it = m_targets.erase(it);
            else ++it;
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
    }
};
