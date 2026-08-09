#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <string>
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

// =================================================================
// ModuleManager
// =================================================================
// THREADING CONTRACT
//
// Tick() runs on our own client thread, which is attached to the
// JVM. It is the ONLY place JNI is allowed.
//
// The render thread (inside the wglSwapBuffers hook) draws the menu
// and reads module state, but never calls JNI. When the user clicks
// a toggle we push it onto a queue and apply it here instead, so
// OnEnable/OnDisable always run with a valid JNIEnv.
//
// Every tick is wrapped in a JNI local frame. Without it the local
// reference table fills up within seconds and the JVM aborts.
// =================================================================

class ModuleManager {
private:
    inline static std::vector<std::shared_ptr<Module>> s_modules;
    inline static bool s_keyStates[256] = {};

    inline static std::vector<Module*> s_pendingToggles;
    inline static std::mutex s_toggleMutex;

    inline static HWND s_gameWindow = nullptr;
    inline static std::shared_ptr<ESP> s_esp;

public:
    static void Init() {
        // ---- Combat: core ----
        s_modules.push_back(std::make_shared<Velocity>());
        s_modules.push_back(std::make_shared<SprintReset>());
        s_modules.push_back(std::make_shared<AutoBlockhit>());
        s_modules.push_back(std::make_shared<AimAssist>());

        // ---- Combat: extra ----
        s_modules.push_back(std::make_shared<HitSelect>());
        s_modules.push_back(std::make_shared<Backtrack>());
        s_modules.push_back(std::make_shared<ClickAssist>());
        s_modules.push_back(std::make_shared<KillAura>());

        // ---- Movement ----
        s_modules.push_back(std::make_shared<BridgeAssist>());
        s_modules.push_back(std::make_shared<Sprint>());
        s_modules.push_back(std::make_shared<NoJumpDelay>());
        s_modules.push_back(std::make_shared<Speed>());
        s_modules.push_back(std::make_shared<Fly>());

        // ---- Visual ----
        s_esp = std::make_shared<ESP>();
        s_modules.push_back(s_esp);
        s_modules.push_back(std::make_shared<Fullbright>());
    }

    static void SetGameWindow(HWND hwnd) { s_gameWindow = hwnd; }

    static std::shared_ptr<ESP> GetESP() { return s_esp; }

    template <typename T>
    static std::shared_ptr<T> Get() {
        for (auto& m : s_modules) {
            auto casted = std::dynamic_pointer_cast<T>(m);
            if (casted) return casted;
        }
        return nullptr;
    }

    // Called from the render thread. Never touches JNI.
    static void QueueToggle(Module* mod) {
        if (!mod) return;
        std::lock_guard<std::mutex> lock(s_toggleMutex);
        s_pendingToggles.push_back(mod);
    }

    static void Tick(JNIEnv* env) {
        if (!env) return;

        // Reserve room for the refs this tick will create. Everything
        // allocated below is released by PopLocalFrame.
        if (env->PushLocalFrame(256) != 0) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return;
        }

        // ---- Apply toggles requested by the UI ----
        {
            std::vector<Module*> pending;
            {
                std::lock_guard<std::mutex> lock(s_toggleMutex);
                pending.swap(s_pendingToggles);
            }
            for (Module* m : pending) m->Toggle(env);
        }

        // ---- Keybinds (only while the game window has focus) ----
        bool focused = (s_gameWindow == nullptr) || (GetForegroundWindow() == s_gameWindow);
        for (auto& mod : s_modules) {
            int key = mod->GetKeybind();
            if (key <= 0 || key > 255) continue;

            bool pressed = focused && ((GetAsyncKeyState(key) & 0x8000) != 0);
            if (pressed && !s_keyStates[key]) mod->Toggle(env);
            s_keyStates[key] = pressed;
        }

        // ---- Run enabled modules ----
        for (auto& mod : s_modules) {
            if (!mod->IsEnabled()) continue;
            mod->OnTick(env);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }

        env->PopLocalFrame(nullptr);
    }

    static void Shutdown(JNIEnv* env) {
        if (env) {
            for (auto& mod : s_modules) {
                if (mod->IsEnabled()) mod->SetEnabled(false, env);
            }
        }
        s_esp.reset();
        s_modules.clear();
    }

    static int GetModuleCount() { return (int)s_modules.size(); }

    static std::vector<std::shared_ptr<Module>>& GetModules() { return s_modules; }

    static std::vector<std::shared_ptr<Module>> GetModulesByCategory(ModuleCategory cat) {
        std::vector<std::shared_ptr<Module>> out;
        for (auto& m : s_modules)
            if (m->GetCategory() == cat) out.push_back(m);
        return out;
    }
};
