#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <cmath>
#include <random>

// =================================================================
// Bridge Assist (AutoEagle)
// =================================================================
// Holds sneak when you reach the edge of the block you are standing
// on, so you can bridge backwards without looking down or falling.
//
// Sneaking is a vanilla input. The packet stream is identical to a
// player tapping shift, so there is nothing here for a server-side
// anticheat to flag.
//
// MODES
//   0 Eagle     sneak at the edge, release once clear
//   1 Godbridge rapid sneak/unsneak for faster bridging
//   2 Breezily  single-tick sneak, fastest and least forgiving
//   3 Safewalk  sneak at any edge, in any direction
// =================================================================

class BridgeAssist : public Module {
private:
    int   m_mode              = 0;
    bool  m_onlyBackward      = true;
    bool  m_onlyHoldingBlocks = false;  // needs the held-item chain
    float m_edgeDistance      = 0.30f;
    int   m_sneakTicks        = 2;
    int   m_unsneakTicks      = 1;

    bool  m_isSneaking  = false;
    int   m_sneakCounter = 0;
    bool  m_atEdge      = false;

    jmethodID m_setSneaking = nullptr;
    bool m_resolved = false;

    void ResolveJNI(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::entity) {
            // setSneaking(boolean) has a primitive signature, but go
            // through JVMTI anyway so obfuscated names still resolve.
            m_setSneaking = JvmtiUtil::FindMethod(env, ClassResolver::entity,
                { "func_70095_a", "setSneaking" }, 1);
        }
        m_resolved = true;
    }

    void SetSneak(JNIEnv* env, jobject player, bool on) {
        if (!m_setSneaking) return;
        env->CallVoidMethod(player, m_setSneaking, (jboolean)on);
        if (env->ExceptionCheck()) env->ExceptionClear();
        m_isSneaking = on;
    }

    // How far into the current block the player is. Being close to a
    // boundary means the next step could take us off the edge.
    bool IsAtEdge(JNIEnv* env, jobject player) {
        double x = Minecraft::GetPosX(env, player);
        double z = Minecraft::GetPosZ(env, player);

        double fx = x - std::floor(x);
        double fz = z - std::floor(z);

        double d = (double)m_edgeDistance;
        bool nearX = (fx < d) || (fx > 1.0 - d);
        bool nearZ = (fz < d) || (fz > 1.0 - d);
        return nearX || nearZ;
    }

public:
    BridgeAssist()
        : Module("Bridge Assist", "Auto-sneak at block edges while bridging",
                 ModuleCategory::MOVEMENT, 0) {}

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) { if (m_isSneaking) SetSneak(env, player, false); return; }

        // Sneaking mid-air looks wrong and does nothing useful
        if (!Minecraft::IsOnGround(env, player)) {
            if (m_isSneaking) SetSneak(env, player, false);
            m_sneakCounter = 0;
            return;
        }

        bool backward = (GetAsyncKeyState('S') & 0x8000) != 0;
        bool anyDir = backward
                   || (GetAsyncKeyState('W') & 0x8000)
                   || (GetAsyncKeyState('A') & 0x8000)
                   || (GetAsyncKeyState('D') & 0x8000);

        bool active = m_onlyBackward ? backward : anyDir;
        if (!active) {
            if (m_isSneaking) SetSneak(env, player, false);
            m_sneakCounter = 0;
            return;
        }

        m_atEdge = IsAtEdge(env, player);

        switch (m_mode) {
            case 1: {   // Godbridge
                if (!m_atEdge) {
                    if (m_isSneaking) SetSneak(env, player, false);
                    m_sneakCounter = 0;
                    break;
                }
                m_sneakCounter++;
                int limit = m_isSneaking ? m_sneakTicks : m_unsneakTicks;
                if (m_sneakCounter >= limit) {
                    SetSneak(env, player, !m_isSneaking);
                    m_sneakCounter = 0;
                }
                break;
            }
            case 2: {   // Breezily: one tick only
                if (m_atEdge) SetSneak(env, player, !m_isSneaking);
                else if (m_isSneaking) SetSneak(env, player, false);
                break;
            }
            default: {  // Eagle and Safewalk share the same logic
                if (m_atEdge && !m_isSneaking)       SetSneak(env, player, true);
                else if (!m_atEdge && m_isSneaking)  SetSneak(env, player, false);
                break;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        if (!m_isSneaking) return;
        jobject player = Minecraft::GetPlayer(env);
        if (player) SetSneak(env, player, false);
        m_isSneaking = false;
        m_sneakCounter = 0;
    }

    void RenderSettings() override {
        const char* modes[] = { "Eagle", "Godbridge", "Breezily", "Safewalk" };
        ImGui::Combo("Mode", &m_mode, modes, 4);

        ImGui::SliderFloat("Edge Distance", &m_edgeDistance, 0.05f, 0.49f, "%.2f");
        ImGui::Checkbox("Only Backward", &m_onlyBackward);

        if (m_mode == 1) {
            ImGui::SliderInt("Sneak Ticks", &m_sneakTicks, 1, 5);
            ImGui::SliderInt("Unsneak Ticks", &m_unsneakTicks, 1, 3);
        }

        ImGui::Separator();
        switch (m_mode) {
            case 0: ImGui::TextWrapped("Eagle: sneak at the edge, release once clear."); break;
            case 1: ImGui::TextWrapped("Godbridge: rapid sneak toggling for speed."); break;
            case 2: ImGui::TextWrapped("Breezily: single-tick sneak. Fastest, least forgiving."); break;
            case 3: ImGui::TextWrapped("Safewalk: sneak at any edge, any direction."); break;
        }

        ImGui::TextDisabled(m_atEdge ? "At edge" : "Clear");

        if (!m_setSneaking) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "setSneaking unresolved: module inactive");
        }
    }
};
