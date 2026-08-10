#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../mc/rotation.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <cmath>
#include <random>
#include <string>

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
// =================================================================

class AimAssist : public Module {
private:
    // ---- Feel ----
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
        Bind("Speed", &m_speed);
        Bind("Pitch Ratio", &m_pitchRatio);
        Bind("Smoothing", &m_smoothing);
        Bind("FOV", &m_fov);
        Bind("Range", &m_range);
        Bind("Target Mode", &m_targetMode);
        Bind("Sticky", &m_sticky);
        Bind("Sticky FOV", &m_stickyFov);
        Bind("Require Swing", &m_requireSwing);
        Bind("Swing Window", &m_swingWindow);
        Bind("Aim Height", &m_aimHeight);
        Bind("Wander", &m_wander);
        Bind("Jitter", &m_jitter);
        Bind("Overshoot", &m_overshoot);
        Bind("Breaks", &m_breaks);
        Bind("Break Chance", &m_breakChance);
        Bind("Break Min", &m_breakMin);
        Bind("Break Max", &m_breakMax);
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
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);
        m_active = false;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        RefreshSensitivity(env);

        // Only help while actually fighting. Tracking someone across
        // a lobby while you stand still is the most visible thing
        // this module could possibly do.
        if (m_requireSwing) {
            bool swinging = CombatState::TicksSinceSwing() <= m_swingWindow;
            bool holding  = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (!swinging && !holding) {
                Rotation::ResetVelocity();
                m_targetId = -1;
                return;
            }
        }

        // Deliberate lapses. Perfect tracking forever is a signature
        // in itself, so the assist drops out now and then.
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

        // Sticky first: switching target mid-exchange produces a
        // whip-round that nothing human does.
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
        // Dead centre every tick is a giveaway, so the point drifts
        // slowly around the body. Re-rolled every few ticks rather
        // than every tick, or it reads as noise instead of drift.
        if (++m_wanderTick >= 7) {
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

        auto next = Rotation::Step(yaw, pitch, want.yaw, want.pitch,
                                   m_speed, m_smoothing,
                                   m_jitter, m_overshoot);

        // Pitch trails yaw. Wrists turn sideways far more readily
        // than they tilt, and matching that is most of why a
        // rotation reads as human.
        float pitchStep = (next.pitch - pitch) * m_pitchRatio;

        Minecraft::SetYaw(env, player, next.yaw);
        Minecraft::SetPitch(env, player, pitch + pitchStep);

        m_active = true;
    }

    void RenderSettings() override {
        // ---- Live ----
        if (m_active && !m_targetName.empty()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.4f, 1.f),
                "Tracking %s  (%.1f deg off)", m_targetName.c_str(), m_lastAngle);
        } else {
            ImGui::TextDisabled("Idle");
        }
        if (Rotation::HaveSensitivity()) {
            ImGui::TextDisabled("Mouse grid %.4f deg  (sens %.2f)",
                Rotation::GCD(), Rotation::Sensitivity());
        } else {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Sensitivity unresolved: using a default grid");
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Feel");
        ImGui::SliderFloat("Speed", &m_speed, 0.5f, 10.f, "%.1f");
        ImGui::SliderFloat("Pitch Ratio", &m_pitchRatio, 0.1f, 1.f, "%.2f");
        ImGui::SliderFloat("Smoothing", &m_smoothing, 0.f, 0.9f, "%.2f");
        ImGui::SliderFloat("FOV", &m_fov, 10.f, 180.f, "%.0f");
        ImGui::SliderFloat("Range", &m_range, 1.f, 6.f, "%.1f");

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Targeting");
        const char* modes[] = { "Crosshair", "Closest", "Lowest HP" };
        ImGui::Combo("Priority", &m_targetMode, modes, 3);
        ImGui::Checkbox("Sticky", &m_sticky);
        if (m_sticky)
            ImGui::SliderFloat("Sticky FOV", &m_stickyFov, 40.f, 200.f, "%.0f");
        ImGui::Checkbox("Only While Swinging", &m_requireSwing);
        if (m_requireSwing)
            ImGui::SliderInt("Swing Window", &m_swingWindow, 2, 20);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 1.f, 0.7f, 1.f), "Aim point");
        ImGui::SliderFloat("Height", &m_aimHeight, 0.f, 1.8f, "%.2f");
        ImGui::SliderFloat("Wander", &m_wander, 0.f, 0.5f, "%.2f");

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Humanisation");
        ImGui::SliderFloat("Jitter", &m_jitter, 0.f, 40.f, "%.0f%%");
        ImGui::SliderFloat("Overshoot", &m_overshoot, 0.f, 0.25f, "%.2f");
        ImGui::Checkbox("Breaks", &m_breaks);
        if (m_breaks) {
            ImGui::SliderFloat("Break Chance", &m_breakChance, 1.f, 25.f, "%.0f%%");
            ImGui::SliderInt("Break Min", &m_breakMin, 1, 8);
            ImGui::SliderInt("Break Max", &m_breakMax, 1, 12);
            if (m_breakMin > m_breakMax) m_breakMin = m_breakMax;
        }

        if (m_speed > 6.f) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "Above 6 the correction is visible in a replay");
        }
        if (m_wander < 0.05f) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Near-zero wander means perfect centre aim every hit");
        }
    }
};
