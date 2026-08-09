#pragma once
#include <vector>
#include <memory>
#include <Windows.h>
#include "module.h"

// Forward declarations
#include "combat/aim_assist.h"
#include "combat/kill_aura.h"
#include "combat/velocity.h"
#include "movement/speed.h"
#include "movement/sprint.h"
#include "movement/fly.h"
#include "visual/esp.h"
#include "visual/fullbright.h"

class ModuleManager {
private:
    inline static std::vector<std::shared_ptr<Module>> s_modules;
    inline static bool s_keyStates[256] = {};

public:
    static void Init() {
        // Combat
        s_modules.push_back(std::make_shared<AimAssist>());
        s_modules.push_back(std::make_shared<KillAura>());
        s_modules.push_back(std::make_shared<Velocity>());

        // Movement
        s_modules.push_back(std::make_shared<Speed>());
        s_modules.push_back(std::make_shared<Sprint>());
        s_modules.push_back(std::make_shared<Fly>());

        // Visual
        s_modules.push_back(std::make_shared<ESP>());
        s_modules.push_back(std::make_shared<Fullbright>());
    }

    static void Tick(JNIEnv* env) {
        // Handle keybinds
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

        // Tick enabled modules
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
