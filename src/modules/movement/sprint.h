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
// Calling entity.setSprinting(true) every tick does not work:
// onLivingUpdate recomputes sprint from the keybind and hunger
// immediately afterwards, so the write is gone before the movement
// packet is built. Holding keyBindSprint is what the game itself
// reads, so the result is identical to a player holding ctrl,
// packets included.
//
// WHY THE HOLD IS RE-ASSERTED EVERY TICK
// It used to press the key only on the rising edge and then trust
// that it stayed down. It does not. Sprint Reset in Ctrl Spam mode
// drives the same keybind, and when it hands the key back it
// restores whatever the hardware says, which is "ctrl is not held"
// because the player is not holding it. Sprint still believed it
// owned the key, never wrote it again, and you silently stopped
// sprinting for the rest of the game.
//
// Writing a boolean that is already true costs nothing, so the
// state is simply re-applied while the conditions hold.
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
        if (!player) {
            if (m_held) { KeyBinds::ReleaseSprint(env); m_held = false; }
            return;
        }

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

        if (moving) {
            // Re-asserted every tick on purpose: another module may
            // have handed this key back to the player since our last
            // write, and the edge-only version never noticed.
            KeyBinds::SetSprint(env, true);
            m_held = true;
        } else if (m_held) {
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
