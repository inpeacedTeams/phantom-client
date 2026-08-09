#pragma once
#include <vector>
#include <memory>
#include <Windows.h>
#include "module.h"

// Combat
#include "combat/aim_assist.h"
#include "combat/kill_aura.h"
#include "combat/velocity.h"
#include "combat/sprint_reset.h"
#include "combat/autoblockhit.h"
#include "combat/hitselect.h"
#include "combat/backtrack.h"
#include "combat/click_assist.h"

// Movement
#include "movement/speed.h"
#include "movement/sprint.h"
#include "movement/fly.h"
#include "movement/bridge_assist.h"
#include "movement/no_jump_delay.h"

// Visual
#include "visual/esp.h"
#include "visual/fullbright.h"

class ModuleManager {
private:
    inline static std::vector<std::shared_ptr<Module>> s_modules;
    inline static bool s_keyStates[256] = {};

public:
    static void Init() {
        // ===== Combat (core) =====
        s_modules.push_back(std::make_shared<Velocity>());       // Reduce incoming KB
        s_modules.push_back(std::make_shared<SprintReset>());    // More outgoing KB
        s_modules.push_back(std::make_shared<AutoBlockhit>());   // Auto sword block
        s_modules.push_back(std::make_shared<AimAssist>());      // Smooth aim correction

        // ===== Combat (extra) =====
        s_modules.push_back(std::make_shared<HitSelect>());      // Click on KB tick
        s_modules.push_back(std::make_shared<Backtrack>());      // Hit past positions
        s_modules.push_back(std::make_shared<ClickAssist>());    // Legit autoclicker
        s_modules.push_back(std::make_shared<KillAura>());       // Auto attack (blatant)

        // ===== Movement (core) =====
        s_modules.push_back(std::make_shared<BridgeAssist>());   // AutoEagle / safewalk
        s_modules.push_back(std::make_shared<Sprint>());         // Always sprint
        s_modules.push_back(std::make_shared<NoJumpDelay>());    // Remove jump cooldown

        // ===== Movement (extra) =====
        s_modules.push_back(std::make_shared<Speed>());          // Speed boost
        s_modules.push_back(std::make_shared<Fly>());            // Fly (blatant)

        // ===== Visual =====
        s_modules.push_back(std::make_shared<ESP>());            // Player wallhack
        s_modules.push_back(std::make_shared<Fullbright>());     // Gamma override
    }

    static void Tick(JNIEnv* env) {
        for (auto& mod : s_modules) {
            int key = mod->GetKeybind();
            if (key > 0) {
                bool pressed = GetAsyncKeyState(key) & 0x8000;
                if (pressed && !s_keyStates[key]) {
                    mod->Toggle(env);
                }
                s_keyStates[key] = pressed;
            }
        }

        for (auto& mod : s_modules) {
            if (mod->IsEnabled()) {
                mod->OnTick(env);
            }
        }
    }

    static void Shutdown() {
        s_modules.clear();
    }

    static int GetModuleCount() {
        return (int)s_modules.size();
    }

    static std::vector<std::shared_ptr<Module>>& GetModules() {
        return s_modules;
    }

    static std::vector<std::shared_ptr<Module>> GetModulesByCategory(ModuleCategory cat) {
        std::vector<std::shared_ptr<Module>> result;
        for (auto& m : s_modules) {
            if (m->GetCategory() == cat) result.push_back(m);
        }
        return result;
    }
};
