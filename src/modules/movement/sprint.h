#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/movement.h"
#include <Windows.h>

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

    void Release(JNIEnv* env) {
        if (!m_held) return;
        if (env) KeyBinds::ReleaseSprint(env);
        m_held = false;
    }

public:
    Sprint() : Module("Sprint", "Always sprint without holding the key",
                      ModuleCategory::MOVEMENT, VK_CONTROL)
    {
        Bind("Omni Sprint", &m_omniSprint,
             "Also sprint sideways and backwards, which vanilla cannot do");
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;
        if (!KeyBinds::HasSprint()) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { Release(env); return; }

        if (Minecraft::IsInGui(env)) { Release(env); return; }

        // Read the game's movement state rather than raw scancodes,
        // so rebound keys and other modules are both accounted for.
        Movement::Input in = Movement::Read(env);
        bool moving = m_omniSprint ? in.moving : (in.forward > 0.0f);

        if (moving) {
            // Re-asserted every tick on purpose: another module may
            // have handed this key back to the player since our last
            // write, and the edge-only version never noticed.
            KeyBinds::SetSprint(env, true);
            m_held = true;
        } else {
            Release(env);
        }
    }

    void OnDisable(JNIEnv* env) override { Release(env); }

    // A reconnect can happen with the key held on our behalf, and
    // the new world would start with a phantom ctrl down.
    void OnReset(JNIEnv* env) override { Release(env); }

    NoticeLevel Notice(const char** text) const override {
        if (!KeyBinds::HasSprint()) {
            *text = "The sprint keybind could not be found in this build of "
                    "the game, so the module cannot do anything.";
            return NoticeLevel::Warning;
        }
        if (m_omniSprint) {
            *text = "Sprinting sideways or backwards is not something vanilla "
                    "can produce, and a prediction anticheat will see it.";
            return NoticeLevel::Warning;
        }
        return NoticeLevel::None;
    }
};
