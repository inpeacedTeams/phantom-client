#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <chrono>
#include <random>

// =================================================================
// Kill Aura
// =================================================================
// Attacks entities in range automatically.
//
// This is the loudest module here. It combines rotation, range and
// timing signals, which is exactly what every modern anticheat is
// built to catch. Leave it off on Polar, AGC and Grim. It exists
// for unprotected servers.
//
// EntityList only ever returns players, so there is no mob filter:
// the target set is players by construction.
// =================================================================

class KillAura : public Module {
private:
    float m_range        = 3.4f;
    int   m_minCPS       = 10;
    int   m_maxCPS       = 14;
    bool  m_rotate       = true;
    bool  m_multiTarget  = false;
    int   m_maxTargets   = 3;
    bool  m_requireClick = false;
    int   m_targetMode   = 0;   // 0 closest, 1 lowest HP

    std::chrono::steady_clock::time_point m_lastAttack;
    long long m_nextDelayMs = 100;
    std::mt19937 m_rng{ std::random_device{}() };

    jmethodID m_attackEntity = nullptr;  // PlayerControllerMP.attackEntity
    jmethodID m_swingItem    = nullptr;  // EntityLivingBase.swingItem
    bool m_resolved = false;

    void ResolveJNI(JNIEnv* env) {
        if (m_resolved) return;

        if (ClassResolver::playerController) {
            m_attackEntity = JvmtiUtil::FindMethod(env, ClassResolver::playerController,
                { "func_78764_a", "attackEntity" }, 2);
        }

        jclass swingHost = ClassResolver::entityLivingBase
                         ? ClassResolver::entityLivingBase
                         : ClassResolver::entity;
        if (swingHost) {
            m_swingItem = JvmtiUtil::FindMethod(env, swingHost,
                { "func_71038_i", "swingItem" }, 0);
        }

        m_resolved = true;
        printf("[KillAura] attack=%p swing=%p\n",
            (void*)m_attackEntity, (void*)m_swingItem);
    }

    long long RollDelay() {
        int lo = m_minCPS < 1 ? 1 : m_minCPS;
        int hi = m_maxCPS < lo ? lo : m_maxCPS;
        std::uniform_int_distribution<int> cps(lo, hi);
        std::uniform_int_distribution<int> jitter(-18, 18);
        long long d = (1000 / cps(m_rng)) + jitter(m_rng);
        return d < 30 ? 30 : d;
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
    }

public:
    KillAura() : Module("Kill Aura", "Auto-attack players in range",
                        ModuleCategory::COMBAT, 'K')
    {
        m_lastAttack = std::chrono::steady_clock::now();

        Bind("Range", &m_range);
        Bind("Min CPS", &m_minCPS);
        Bind("Max CPS", &m_maxCPS);
        Bind("Rotate", &m_rotate);
        Bind("Multi Target", &m_multiTarget);
        Bind("Max Targets", &m_maxTargets);
        Bind("Only While Clicking", &m_requireClick);
        Bind("Target Mode", &m_targetMode);
    }

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);
        if (!m_attackEntity) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;
        if (m_requireClick && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastAttack).count();
        if (elapsed < m_nextDelayMs) return;

        if (!EntityList::Init(env)) return;
        auto ents = EntityList::GetPlayers(env, m_range);
        if (ents.empty()) return;

        int hit = 0;

        if (m_multiTarget) {
            for (auto& e : ents) {
                if (hit >= m_maxTargets) break;
                if (e.distanceToPlayer > m_range) continue;
                Attack(env, player, e.ref);
                hit++;
            }
        } else {
            EntityInfo* t = (m_targetMode == 1)
                ? EntityList::FindLowestHP(ents, m_range)
                : EntityList::FindClosest(ents, m_range);
            if (!t) return;

            if (m_rotate) {
                auto rot = Minecraft::GetRotationsToPos(env, player,
                    t->posX, t->posY + 1.0, t->posZ);
                Minecraft::SetYaw(env, player, rot.yaw);
                Minecraft::SetPitch(env, player, rot.pitch);
            }
            Attack(env, player, t->ref);
            hit = 1;
        }

        if (hit > 0) {
            m_lastAttack = now;
            m_nextDelayMs = RollDelay();
        }
    }

    void RenderSettings() override {
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f),
            "! Detected by Polar, AGC and Grim");
        ImGui::Separator();

        ImGui::SliderFloat("Range", &m_range, 2.f, 6.f, "%.1f blocks");
        ImGui::SliderInt("Min CPS", &m_minCPS, 1, 20);
        ImGui::SliderInt("Max CPS", &m_maxCPS, 1, 20);
        if (m_minCPS > m_maxCPS) m_minCPS = m_maxCPS;

        const char* modes[] = { "Closest", "Lowest HP" };
        ImGui::Combo("Target", &m_targetMode, modes, 2);

        ImGui::Checkbox("Rotate To Target", &m_rotate);
        ImGui::Checkbox("Only While Clicking", &m_requireClick);
        ImGui::Checkbox("Multi Target", &m_multiTarget);
        if (m_multiTarget) ImGui::SliderInt("Max Targets", &m_maxTargets, 2, 8);

        if (!m_attackEntity) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "attackEntity unresolved: join a world first");
        }
    }
};
