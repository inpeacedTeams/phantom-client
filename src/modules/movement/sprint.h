#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include <imgui.h>

// =================================================================
// Sprint
// =================================================================
// Keeps you sprinting without holding the key.
//
// The old version called entity.setSprinting(true) every tick. That
// does not work: onLivingUpdate recomputes sprint from the keybind
// and hunger immediately afterwards, so the write was gone before
// the movement packet was built.
//
// Holding keyBindSprint is what the game itself reads, so the result
// is identical to a player holding ctrl, packets included.
// =================================================================

class Sprint : public Module {
private:
    bool m_omniSprint = false;   // sprint in any direction, not just forward
    bool m_held = false;

public:
    Sprint() : Module("Sprint", "Always sprint without holding the key",
                      ModuleCategory::MOVEMENT, VK_CONTROL)
    {
        Bind("Omni Sprint", &m_omniSprint);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;
        if (!KeyBinds::HasSprint()) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (Minecraft::IsInGui(env)) {
            if (m_held) { KeyBinds::ReleaseSprint(env); m_held = false; }
            return;
        }

        // Read the game's movement state rather than raw scancodes,
        // so rebound keys and other modules are both accounted for.
        bool moving = m_omniSprint
            ? (KeyBinds::GetForward(env) || KeyBinds::GetBack(env)
            || KeyBinds::GetLeft(env)    || KeyBinds::GetRight(env))
            :  KeyBinds::GetForward(env);

        if (moving && !m_held) {
            KeyBinds::SetSprint(env, true);
            m_held = true;
        } else if (!moving && m_held) {
            KeyBinds::ReleaseSprint(env);
            m_held = false;
        }
    }

    void OnDisable(JNIEnv* env) override {
        if (!m_held) return;
        KeyBinds::ReleaseSprint(env);
        m_held = false;
    }

    void RenderSettings() override {
        ImGui::Checkbox("Omni Sprint", &m_omniSprint);
        if (m_omniSprint) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Sprinting sideways or backwards is not vanilla behaviour");
        }
        if (!KeyBinds::HasSprint()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Sprint keybind unresolved: module inactive");
        }
    }
};
