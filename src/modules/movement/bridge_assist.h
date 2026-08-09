#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include <imgui.h>
#include <Windows.h>
#include <cmath>

// =================================================================
// Bridge Assist (AutoEagle)
// =================================================================
// Holds sneak when you reach the edge of the block under you, so
// you can bridge backwards without looking down or falling.
//
// Driving keyBindSneak rather than setSneaking matters: the entity
// flag is recomputed from the key every tick, so writing it直接
// achieves nothing. Holding the key is also what makes this
// undetectable, since the packet stream is a plain shift hold.
//
// MODES
//   0 Eagle     sneak at the edge, release once clear
//   1 Godbridge rapid sneak toggling for speed
//   2 Breezily  single-tick sneak, fastest and least forgiving
//   3 Safewalk  sneak at any edge, in any direction
// =================================================================

class BridgeAssist : public Module {
private:
    int   m_mode         = 0;
    bool  m_onlyBackward = true;
    float m_edgeDistance = 0.30f;
    int   m_sneakTicks   = 2;
    int   m_unsneakTicks = 1;

    bool m_sneaking     = false;
    int  m_sneakCounter = 0;
    bool m_atEdge       = false;

    void SetSneak(JNIEnv* env, bool on) {
        if (m_sneaking == on) return;
        KeyBinds::SetSneak(env, on);
        m_sneaking = on;
    }

    // How deep into the current block we are. Sitting near a
    // boundary means the next step could walk us off it.
    bool IsAtEdge(JNIEnv* env, jobject player) {
        double x = Minecraft::GetPosX(env, player);
        double z = Minecraft::GetPosZ(env, player);
        double fx = x - std::floor(x);
        double fz = z - std::floor(z);
        double d = (double)m_edgeDistance;
        return (fx < d) || (fx > 1.0 - d) || (fz < d) || (fz > 1.0 - d);
    }

public:
    BridgeAssist()
        : Module("Bridge Assist", "Auto-sneak at block edges while bridging",
                 ModuleCategory::MOVEMENT, 0)
    {
        Bind("Mode", &m_mode);
        Bind("Only Backward", &m_onlyBackward);
        Bind("Edge Distance", &m_edgeDistance);
        Bind("Sneak Ticks", &m_sneakTicks);
        Bind("Unsneak Ticks", &m_unsneakTicks);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (Minecraft::IsInGui(env)) { SetSneak(env, false); return; }

        // Sneaking mid-air looks wrong and buys nothing
        if (!Minecraft::IsOnGround(env, player)) {
            SetSneak(env, false);
            m_sneakCounter = 0;
            return;
        }

        bool back = KeyBinds::GetBack(env);
        bool any  = back || KeyBinds::GetForward(env)
                 || KeyBinds::GetLeft(env) || KeyBinds::GetRight(env);

        bool active = m_onlyBackward ? back : any;
        if (!active) {
            SetSneak(env, false);
            m_sneakCounter = 0;
            return;
        }

        m_atEdge = IsAtEdge(env, player);

        switch (m_mode) {
            case 1: {   // Godbridge
                if (!m_atEdge) { SetSneak(env, false); m_sneakCounter = 0; break; }
                m_sneakCounter++;
                int limit = m_sneaking ? m_sneakTicks : m_unsneakTicks;
                if (m_sneakCounter >= limit) {
                    SetSneak(env, !m_sneaking);
                    m_sneakCounter = 0;
                }
                break;
            }
            case 2: {   // Breezily
                if (m_atEdge) SetSneak(env, !m_sneaking);
                else          SetSneak(env, false);
                break;
            }
            default: {  // Eagle and Safewalk
                SetSneak(env, m_atEdge);
                break;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        SetSneak(env, false);
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
            case 0: ImGui::TextWrapped("Sneak at the edge, release once clear."); break;
            case 1: ImGui::TextWrapped("Rapid sneak toggling for faster bridging."); break;
            case 2: ImGui::TextWrapped("Single-tick sneak. Fastest, least forgiving."); break;
            case 3: ImGui::TextWrapped("Sneak at any edge, any direction."); break;
        }
        ImGui::TextDisabled(m_atEdge ? "At edge" : "Clear");

        if (!KeyBinds::HasSneak()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Sneak keybind unresolved: module inactive");
        }
    }
};
