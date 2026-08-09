#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include <imgui.h>
#include <Windows.h>

// =================================================================
// Sprint
// =================================================================
// Holds the sprint keybind so you never have to.
//
// Calling setSprinting(true) directly does not survive the tick:
// EntityPlayerSP.onLivingUpdate() recomputes sprint from the keys
// and clears it. Holding keyBindSprint sits upstream of that and is
// byte-for-byte what a player with sprint bound would produce.
// =================================================================

class Sprint : public Module {
private:
    bool m_omniSprint = false;   // sprint sideways and backwards too
    bool m_held = false;

public:
    Sprint() : Module("Sprint", "Always sprint without holding the key",
                      ModuleCategory::MOVEMENT, VK_CONTROL)
    {
        Bind("Omni Sprint", &m_omniSprint);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) {
            if (m_held) { KeyBinds::SetSprint(env, false); m_held = false; }
            return;
        }

        bool moving = m_omniSprint
            ? (KeyBinds::GetForward(env) || KeyBinds::GetBack(env)
            || KeyBinds::GetLeft(env)    || KeyBinds::GetRight(env))
            :  KeyBinds::GetForward(env);

        if (moving && !m_held) {
            KeyBinds::SetSprint(env, true);
            m_held = true;
        } else if (!moving && m_held) {
            KeyBinds::SetSprint(env, false);
            m_held = false;
        }
    }

    void OnDisable(JNIEnv* env) override {
        if (!m_held) return;
        KeyBinds::SetSprint(env, false);
        m_held = false;
    }

    void RenderSettings() override {
        ImGui::Checkbox("Omni Sprint", &m_omniSprint);
        if (m_omniSprint) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Sideways sprint is not vanilla behaviour");
        }
    }
};
