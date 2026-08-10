#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../mc/rotation.h"
#include "../../input/focus.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include "../../util/log.h"
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <functional>

// =================================================================
// Kill Aura
// =================================================================
// Attacks players in range automatically. This is the loudest
// module in the client and nothing here makes it safe on a real
// anticheat. What the rewrite does is remove the free wins an
// anticheat used to get:
//
//   * rotations snapped straight to the target, off the mouse grid
//   * the attack packet arriving on the SAME tick as the rotation
//   * a hitbox-centre aim point, every time, forever
//   * swinging with no regard for the 1.8 attack cooldown
//
// AIM BEFORE YOU SWING
// A human turns, then hits. Sending both in one tick means the
// server receives a rotation and a hit with zero time between
// them, which is trivially checked and is what most naive auras
// die to. This one turns first and only attacks once the crosshair
// has actually arrived, which also means it respects FOV honestly.
// =================================================================

class KillAura : public Module {
private:
    static constexpr const char* kPriorities[] = {
        "Closest", "Lowest HP", "Crosshair"
    };

    // 1.8 grants ten ticks of damage immunity after a hit. Swinging
    // into the tail of that window deals nothing.
    static constexpr jint kImmunityGuard = 5;

    // Nothing human clicks faster than this, whatever the CPS says.
    static constexpr long long kFloorMs = 45;

    static constexpr int kWanderPeriod = 6;

    // ---- Targeting ----
    float m_range      = 3.2f;
    float m_fov        = 120.0f;
    int   m_targetMode = 0;      // 0 closest, 1 lowest HP, 2 crosshair
    bool  m_sticky     = true;

    // ---- Rate ----
    int  m_minCPS = 9;
    int  m_maxCPS = 13;
    bool m_respectCooldown = true;   // 1.8 hurt-resistance window

    // ---- Rotation ----
    bool  m_rotate      = true;
    float m_rotSpeed    = 4.5f;
    float m_rotSmooth   = 0.4f;
    float m_maxAimError = 12.0f;  // degrees before a swing is allowed
    float m_aimHeight   = 1.1f;
    float m_wander      = 0.15f;

    // ---- Gating ----
    bool m_requireClick = false;
    bool m_multiTarget  = false;
    int  m_maxTargets   = 3;

    // ---- State ----
    std::chrono::steady_clock::time_point m_lastAttack;
    long long m_nextDelayMs = 100;
    int    m_targetId = -1;
    double m_wanderX = 0, m_wanderY = 0, m_wanderZ = 0;
    int    m_wanderTick = 0;

    // Readout
    std::string m_targetName;
    float m_aimError = 0.0f;
    int   m_hits = 0;
    const char* m_why = "idle";
    mutable char m_status[80] = "";

    jmethodID m_attackEntity = nullptr;
    jmethodID m_swingItem    = nullptr;
    jmethodID m_getEntityId  = nullptr;
    jfieldID  m_fHurtResist  = nullptr;   // target's damage immunity
    jfieldID  m_fSens        = nullptr;
    bool m_resolved = false;

    std::mt19937 m_rng{ std::random_device{}() };

    float Randf(float lo, float hi) {
        std::uniform_real_distribution<float> d(lo, hi);
        return d(m_rng);
    }

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;

        if (ClassResolver::playerController) {
            m_attackEntity = JvmtiUtil::FindMethod(env,
                ClassResolver::playerController,
                { "func_78764_a", "attackEntity" }, 2);
        }

        jclass host = ClassResolver::entityLivingBase
                    ? ClassResolver::entityLivingBase
                    : ClassResolver::entity;
        if (host) {
            m_swingItem = JvmtiUtil::FindMethod(env, host,
                { "func_71038_i", "swingItem" }, 0);
        }
        if (ClassResolver::entity) {
            m_getEntityId = JvmtiUtil::FindMethod(env, ClassResolver::entity,
                { "func_145782_y", "getEntityId" }, 0);
            m_fHurtResist = JvmtiUtil::FindField(env, ClassResolver::entity,
                { "field_70172_ad", "hurtResistantTime" });
        }
        if (ClassResolver::gameSettings) {
            m_fSens = JvmtiUtil::FindField(env, ClassResolver::gameSettings,
                { "field_74341_c", "mouseSensitivity" });
        }

        m_resolved = true;

        // PH_LOG rather than printf: in a release build there is no
        // console attached, and a blocking write to a handle nobody
        // is reading is not free.
        PH_LOG("[KillAura] attack=%p swing=%p resist=%p",
            (void*)m_attackEntity, (void*)m_swingItem, (void*)m_fHurtResist);
    }

    void RefreshSensitivity(JNIEnv* env) {
        if (!m_fSens) return;
        jobject gs = Minecraft::GetGameSettings(env);
        if (gs) Rotation::SetSensitivity(env->GetFloatField(gs, m_fSens));
    }

    int EntityId(JNIEnv* env, jobject ref, const std::string& name) {
        if (m_getEntityId) {
            jint v = env->CallIntMethod(ref, m_getEntityId);
            if (!env->ExceptionCheck()) return (int)v;
            env->ExceptionClear();
        }
        return (int)std::hash<std::string>{}(name);
    }

    long long RollDelay() {
        int lo = m_minCPS < 1 ? 1 : m_minCPS;
        int hi = m_maxCPS < lo ? lo : m_maxCPS;
        std::uniform_int_distribution<int> cps(lo, hi);
        std::uniform_int_distribution<int> jitter(-22, 22);
        long long d = (1000 / cps(m_rng)) + jitter(m_rng);
        return d < kFloorMs ? kFloorMs : d;
    }

    bool OnCooldown(JNIEnv* env, jobject ent) {
        if (!m_respectCooldown || !m_fHurtResist) return false;
        jint r = env->GetIntField(ent, m_fHurtResist);
        return r > kImmunityGuard;
    }

    float AngleTo(JNIEnv* env, jobject player,
                  double x, double y, double z, float yaw, float pitch)
    {
        auto r = Minecraft::GetRotationsToPos(env, player, x, y, z);
        float dy = Rotation::Wrap(r.yaw - yaw);
        float dp = r.pitch - pitch;
        return std::sqrt(dy * dy + dp * dp);
    }

    void Attack(JNIEnv* env, jobject player, jobject target) {
        if (!m_attackEntity) return;

        jobject controller = Minecraft::GetPlayerController(env);
        if (!controller) return;

        env->CallVoidMethod(controller, m_attackEntity, player, target);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

        if (m_swingItem) {
            env->CallVoidMethod(player, m_swingItem);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        m_hits++;
    }

public:
    KillAura() : Module("Kill Aura", "Auto-attack players in range",
                        ModuleCategory::COMBAT, 'K')
    {
        m_lastAttack = std::chrono::steady_clock::now();

        BindMode("Priority", &m_targetMode, kPriorities, 3,
                 "Which of several people in range it goes for");

        Bind("Range", &m_range, 2.0f, 6.0f, "%.2f",
             "Vanilla reach is 3.0. Past that is the single easiest thing "
             "for a server to catch.");

        Bind("Min CPS", &m_minCPS, 1, 20,
             "Slowest it will swing");

        Bind("Max CPS", &m_maxCPS, 1, 20,
             "Fastest it will swing");

        // ---- Targeting ----
        Bind("FOV", &m_fov, 30.0f, 360.0f, "%.0f",
             "How far off centre a target can be. 360 means it hits people "
             "behind you, which nothing human does.")
            .Advanced();

        Bind("Sticky", &m_sticky,
             "Stay on one target rather than re-picking every tick")
            .Advanced();

        Bind("Respect Hurt Cooldown", &m_respectCooldown,
             "Skip targets still immune from the last hit. Swinging into "
             "that window deals nothing and only adds packets.")
            .Advanced();

        // ---- Rotation ----
        Bind("Rotate To Target", &m_rotate,
             "Turn the camera. Without this the hits land at angles you are "
             "not looking at, which is far louder.")
            .Advanced();

        Bind("Rotation Speed", &m_rotSpeed, 1.0f, 12.0f, "%.1f")
            .When("Rotate To Target", 1).Advanced();

        Bind("Rotation Smoothing", &m_rotSmooth, 0.0f, 0.9f, "%.2f")
            .When("Rotate To Target", 1).Advanced();

        Bind("Max Aim Error", &m_maxAimError, 1.0f, 45.0f, "%.0f",
             "Degrees. It will not swing until the crosshair is this close, "
             "so the rotation and the hit are never the same tick.")
            .When("Rotate To Target", 1).Advanced();

        Bind("Aim Height", &m_aimHeight, 0.0f, 1.8f, "%.2f",
             "Metres up the body")
            .When("Rotate To Target", 1).Advanced();

        Bind("Wander", &m_wander, 0.0f, 0.4f, "%.2f",
             "Drift around the aim point, so it is never dead centre")
            .When("Rotate To Target", 1).Advanced();

        // ---- Gating ----
        Bind("Only While Clicking", &m_requireClick,
             "Only run while you hold the button")
            .Advanced();

        Bind("Multi Target", &m_multiTarget,
             "Hit several people in one swing, which nothing human does")
            .Advanced();

        Bind("Max Targets", &m_maxTargets, 2, 8)
            .When("Multi Target", 1).Advanced();
    }

    void OnEnable(JNIEnv*) override {
        Rotation::ResetVelocity();
        m_targetId = -1;
    }

    void OnDisable(JNIEnv*) override {
        Rotation::ResetVelocity();
        m_targetId = -1;
        m_targetName.clear();
        m_why = "off";
        m_status[0] = '\0';
    }

    void OnReset(JNIEnv*) override {
        Rotation::ResetVelocity();
        m_targetId = -1;
        m_targetName.clear();
        m_hits = 0;
        m_why = "idle";
        m_lastAttack = std::chrono::steady_clock::now();
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);
        if (!m_attackEntity) { m_why = "unresolved"; return; }

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) { m_why = "menu"; return; }

        if (m_minCPS > m_maxCPS) m_minCPS = m_maxCPS;

        // Focus::KeyHeld, not a bare GetAsyncKeyState: the latter is
        // global and would let a click in another app while
        // alt-tabbed count as holding the button.
        if (m_requireClick && !Focus::KeyHeld(VK_LBUTTON)) {
            m_why = "not holding";
            return;
        }

        RefreshSensitivity(env);

        if (!EntityList::Init(env)) return;
        auto ents = EntityList::GetPlayers(env, m_range + 1.0f);
        if (ents.empty()) {
            m_targetId = -1;
            m_targetName.clear();
            m_why = "no targets";
            return;
        }

        float yaw   = Minecraft::GetYaw(env, player);
        float pitch = Minecraft::GetPitch(env, player);

        // ---- Pick a target ----
        EntityInfo* chosen = nullptr;

        if (m_sticky && m_targetId >= 0) {
            for (auto& e : ents) {
                if (EntityId(env, e.ref, e.name) != m_targetId) continue;
                if (e.distanceToPlayer <= m_range && !OnCooldown(env, e.ref))
                    chosen = &e;
                break;
            }
        }

        if (!chosen) {
            float best = 1e9f;
            for (auto& e : ents) {
                if (e.distanceToPlayer > m_range) continue;
                if (OnCooldown(env, e.ref)) continue;

                float a = AngleTo(env, player, e.posX, e.posY + m_aimHeight,
                                  e.posZ, yaw, pitch);
                if (a > m_fov * 0.5f) continue;

                float score;
                switch (m_targetMode) {
                    case 1:  score = e.health; break;
                    case 2:  score = a; break;
                    default: score = (float)e.distanceToPlayer; break;
                }
                if (score < best) { best = score; chosen = &e; }
            }
        }

        if (!chosen) {
            m_targetId = -1;
            m_targetName.clear();
            Rotation::ResetVelocity();
            m_why = "nothing hittable";
            return;
        }

        m_targetId = EntityId(env, chosen->ref, chosen->name);
        m_targetName = chosen->name;

        // ---- Aim point ----
        if (++m_wanderTick >= kWanderPeriod) {
            m_wanderTick = 0;
            m_wanderX = Randf(-m_wander, m_wander);
            m_wanderY = Randf(-m_wander * 0.6f, m_wander * 0.6f);
            m_wanderZ = Randf(-m_wander, m_wander);
        }

        double tx = chosen->posX + m_wanderX;
        double ty = chosen->posY + m_aimHeight + m_wanderY;
        double tz = chosen->posZ + m_wanderZ;

        m_aimError = AngleTo(env, player, tx, ty, tz, yaw, pitch);

        // ---- Turn ----
        if (m_rotate) {
            auto want = Rotation::ToPoint(env, player, tx, ty, tz);
            auto next = Rotation::Step(yaw, pitch, want.yaw, want.pitch,
                                       m_rotSpeed, m_rotSmooth, 10.0f, 0.04f);
            Minecraft::SetYaw(env, player, next.yaw);
            Minecraft::SetPitch(env, player, next.pitch);

            m_aimError = AngleTo(env, player, tx, ty, tz, next.yaw, next.pitch);
        }

        // ---- Swing, but only once the crosshair has arrived ----
        if (m_rotate && m_aimError > m_maxAimError) {
            m_why = "turning";
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastAttack).count();
        if (elapsed < m_nextDelayMs) { m_why = "on cooldown"; return; }

        int hit = 0;
        if (m_multiTarget) {
            for (auto& e : ents) {
                if (hit >= m_maxTargets) break;
                if (e.distanceToPlayer > m_range) continue;
                if (OnCooldown(env, e.ref)) continue;
                Attack(env, player, e.ref);
                hit++;
            }
        } else {
            Attack(env, player, chosen->ref);
            hit = 1;
        }

        if (hit > 0) {
            m_lastAttack = now;
            m_nextDelayMs = RollDelay();
            m_why = "attacking";
        }
    }

    const char* StatusLine() const override {
        if (!m_targetName.empty()) {
            snprintf(m_status, sizeof(m_status), "%s  \xc2\xb7  %s",
                     m_targetName.c_str(), m_why);
        } else {
            snprintf(m_status, sizeof(m_status), "%s  \xc2\xb7  %d hits",
                     m_why, m_hits);
        }
        return m_status;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!m_attackEntity) {
            *text = "The attack method has not been found yet. Join a world "
                    "and it will resolve itself.";
            return NoticeLevel::Warning;
        }
        *text = "This is the loudest module in the client. Polar, AGC and "
                "Grim all catch it, and nothing in here changes that. "
                "Unprotected servers only.";
        return NoticeLevel::Danger;
    }
};
