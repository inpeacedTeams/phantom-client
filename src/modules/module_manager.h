#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <functional>
#include <unordered_map>
#include <Windows.h>
#include "module.h"
#include "../mc/keybinds.h"
#include "../mc/entity_list.h"
#include "../mc/combat_state.h"
#include "../mc/rotation.h"
#include "../mc/mouse_control.h"
#include "../input/key_capture.h"
#include "../input/focus.h"
#include "../render/camera.h"
#include "../gui/notifications.h"

// Combat
#include "combat/velocity.h"
#include "combat/sprint_reset.h"

// Movement
#include "movement/no_jump_delay.h"

// Visual
#include "visual/esp.h"
#include "visual/fullbright.h"

// =================================================================
// ModuleManager
// =================================================================
// THREADING CONTRACT
//
// Tick() runs on our client thread, which is attached to the JVM.
// It is the only place JNI may be touched.
//
// The render thread draws the menu and reads module state but never
// calls JNI. Anything the UI wants to do that needs a JNIEnv is
// pushed onto the action queue and executed here instead. That
// includes opening the menu itself: releasing the mouse grab is a
// call into Minecraft, so the window thread only sets a flag and
// this tick acts on it.
//
// Every tick runs inside a JNI local frame. Without it the local
// reference table fills up within seconds and the JVM aborts.
//
// TICK ORDER  (all of it is load-bearing)
//
//   1. Push the local frame, invalidate the entity cache.
//   2. Lifecycle: has the world changed under us, did we just die?
//   3. Camera snapshot, taken before any module rotates the player.
//   4. CombatState, so every module reads the same facts about this
//      tick rather than each deriving its own from the mouse.
//   5. Mouse grab follows the menu.
//   6. UI actions, keybinds, then the modules themselves. While the
//      menu is open the gameplay modules are held down, because the
//      cursor is the UI's and their input would be aimed at it.
//   7. Key reconciliation, after everything has had its say.
//
// KEY SAFETY
// Modules hold real keybinds down, and KeyBinding.pressed is edge
// driven: the game only writes it when the keyboard fires an event.
// A key we clear while the player is holding it therefore stays
// cleared forever, and the player simply stops moving. That is what
// froze everyone after a sprint reset.
//
// So there are two backstops. Every exit path releases held keys,
// and every tick ends with KeyBinds::Reconcile, which puts any key
// we are no longer driving back in line with the hardware. Worst
// case a key is wrong for one tick.
//
// LIFECYCLE
// A tick is not the only thing that happens to a module. The world
// gets swapped when you change servers or take a portal, the player
// dies and respawns, the connection drops and comes back. In every
// one of those cases the module is still enabled and still running,
// but everything it remembers is about a situation that no longer
// exists.
//
// Rather than have each module try to notice, the manager watches
// for it once and calls OnReset on all of them.
//
// FAULT ISOLATION
// A module that throws is counted; a few faults in a row means it is
// broken rather than unlucky, so it is switched off and the user is
// told which one and why. The rest of the client keeps running.
// =================================================================

class ModuleManager {
private:
    inline static std::vector<std::shared_ptr<Module>> s_modules;
    inline static bool s_keyStates[256] = {};

    // Hotkeys are suppressed while the menu is open or a bind is
    // being captured. Coming back from that, a key the player is
    // still holding must not read as a fresh press.
    inline static bool s_hotkeysWere = false;

    inline static std::vector<std::function<void(JNIEnv*)>> s_actions;
    inline static std::mutex s_actionMutex;

    inline static HWND s_gameWindow = nullptr;
    inline static std::shared_ptr<ESP> s_esp;
    inline static bool s_wasInGame = false;

    // ---- Lifecycle tracking ----
    // A global ref to the world we last saw. Comparing the object
    // itself is the only reliable signal: a dimension change and a
    // server switch both replace WorldClient, and neither announces
    // itself anywhere we can read.
    inline static jobject s_lastWorld = nullptr;
    inline static bool    s_wasDead   = false;

    // Consecutive faults per module. Reset by a clean tick, because
    // one bad frame during a world change is not a broken module.
    inline static std::unordered_map<std::string, int> s_faults;
    static constexpr int kFaultLimit = 5;

    // Written by the window thread, read here. The menu itself lives
    // on the render side; this is only how the client thread learns
    // that it should hand the cursor back to the player.
    inline static std::atomic<bool> s_menuOpen{ false };

    // Latches the menu-open edge on the client thread. While the
    // menu is up the cursor belongs to the UI, so gameplay modules
    // stand down: one that kept moving the camera would be acting on
    // input the player is aiming at a switch. The edge is where held
    // keys are let go, once.
    inline static bool s_gameplayHalted = false;

    // A packed lobby can hold 60+ players, and the entity scan takes
    // a couple of refs each on top of whatever the modules allocate.
    static constexpr jint kLocalFrameSize = 768;

    // -------------------------------------------------------------
    // Swallow a pending Java exception and say whether there was one.
    // -------------------------------------------------------------
    static bool ClearJavaException(JNIEnv* env) {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    }

    // -------------------------------------------------------------
    // Run one module's tick with a net under it.
    // -------------------------------------------------------------
    static void RunModule(JNIEnv* env, const std::shared_ptr<Module>& mod) {
        const std::string& name = mod->GetName();
        bool faulted = false;
        const char* reason = "threw an exception";

        try {
            mod->OnTick(env);
            if (ClearJavaException(env)) {
                faulted = true;
                reason = "a call into Minecraft failed";
            }
        } catch (const std::exception&) {
            ClearJavaException(env);
            faulted = true;
        } catch (...) {
            ClearJavaException(env);
            faulted = true;
        }

        if (!faulted) {
            auto it = s_faults.find(name);
            if (it != s_faults.end()) s_faults.erase(it);
            return;
        }

        int count = ++s_faults[name];
        if (count < kFaultLimit) return;

        s_faults.erase(name);
        try {
            mod->SetEnabled(false, env);
            ClearJavaException(env);
        } catch (...) {
            ClearJavaException(env);
        }
        KeyBinds::ReleaseAll(env);
        ClearJavaException(env);

        iOS::Notify::Error(name + " was disabled",
            std::string("It ") + reason +
            " repeatedly. The rest of the client is unaffected.");
    }

    // -------------------------------------------------------------
    // Hand every module a clean slate. Enabled modules stay enabled.
    // -------------------------------------------------------------
    static void BroadcastReset(JNIEnv* env) {
        CombatState::Reset();
        Rotation::ResetVelocity();
        Camera::Invalidate();
        EntityList::BeginTick();
        s_faults.clear();

        for (auto& mod : s_modules) {
            try {
                mod->OnReset(env);
            } catch (...) {}
            ClearJavaException(env);
        }

        KeyBinds::ReleaseAll(env);
        ClearJavaException(env);
    }

    // -------------------------------------------------------------
    // Watch for the ground moving: world swap, death, respawn.
    // -------------------------------------------------------------
    static void UpdateLifecycle(JNIEnv* env) {
        jobject world = Minecraft::GetWorld(env);
        ClearJavaException(env);

        bool changed = false;

        if (!world) {
            if (s_lastWorld) {
                env->DeleteGlobalRef(s_lastWorld);
                s_lastWorld = nullptr;
                changed = true;
            }
        } else if (!s_lastWorld || !env->IsSameObject(s_lastWorld, world)) {
            if (s_lastWorld) {
                env->DeleteGlobalRef(s_lastWorld);
                changed = true;   // a swap, not the first world we saw
            }
            s_lastWorld = env->NewGlobalRef(world);
            ClearJavaException(env);
        }

        jobject player = Minecraft::GetPlayer(env);
        ClearJavaException(env);

        bool dead = player && Minecraft::IsDead(env, player);
        if (dead != s_wasDead) {
            s_wasDead = dead;
            changed = true;
        }

        if (changed) BroadcastReset(env);
    }

public:
    static void Init() {
        // Combat
        s_modules.push_back(std::make_shared<Velocity>());
        s_modules.push_back(std::make_shared<SprintReset>());

        // Movement
        s_modules.push_back(std::make_shared<NoJumpDelay>());

        // Visual
        s_esp = std::make_shared<ESP>();
        s_modules.push_back(s_esp);
        s_modules.push_back(std::make_shared<Fullbright>());
    }

    static void SetGameWindow(HWND hwnd) {
        s_gameWindow = hwnd;
        // The shared Focus helper answers "does the game have focus"
        // for the modules that read the mouse directly.
        Focus::SetWindow(hwnd);
    }

    // Called from the window thread when INSERT is pressed. Does no
    // work itself: releasing the grab is a JNI call and belongs on
    // the client thread, which reads s_menuOpen next tick.
    static void SetMenuOpen(bool open) { s_menuOpen.store(open); }
    static bool IsMenuOpen()           { return s_menuOpen.load(); }

    static std::shared_ptr<ESP> GetESP() { return s_esp; }

    template <typename T>
    static std::shared_ptr<T> Get() {
        for (auto& m : s_modules) {
            auto casted = std::dynamic_pointer_cast<T>(m);
            if (casted) return casted;
        }
        return nullptr;
    }

    static Module* Find(const std::string& name) {
        for (auto& m : s_modules)
            if (m->GetName() == name) return m.get();
        return nullptr;
    }

    // Called from the render thread. Never touches JNI itself.
    static void QueueAction(std::function<void(JNIEnv*)> fn) {
        if (!fn) return;
        std::lock_guard<std::mutex> lock(s_actionMutex);
        s_actions.push_back(std::move(fn));
    }

    static void QueueToggle(Module* mod) {
        if (!mod) return;
        QueueAction([mod](JNIEnv* env) { mod->Toggle(env); });
    }

    // Disconnecting mid-fight used to leave forward or sneak held,
    // which follows you into the next game. Modules stay enabled;
    // only the physical key state is dropped.
    static void OnLeaveWorld(JNIEnv* env) {
        if (!s_wasInGame) return;
        s_wasInGame = false;

        Camera::Invalidate();       // overlays stop drawing stale geometry
        CombatState::Reset();       // swing history from the last server is meaningless
        Rotation::ResetVelocity();  // or the arm keeps drifting in the next game
        MouseControl::Shutdown();   // the next world re-resolves and re-grabs
        s_faults.clear();           // a world change is not a module fault
        s_wasDead = false;
        s_gameplayHalted = false;   // re-entry re-evaluates the menu edge cleanly

        if (env) {
            for (auto& mod : s_modules) {
                try {
                    mod->OnReset(env);
                } catch (...) {}
                ClearJavaException(env);
            }

            if (s_lastWorld) {
                env->DeleteGlobalRef(s_lastWorld);
                s_lastWorld = nullptr;
            }

            KeyBinds::ReleaseAll(env);
            ClearJavaException(env);
        }
    }

    static void Tick(JNIEnv* env) {
        if (!env) return;
        s_wasInGame = true;

        if (env->PushLocalFrame(kLocalFrameSize) != 0) {
            ClearJavaException(env);
            return;
        }

        // Entity refs belong to the frame we just pushed, so the
        // cache has to be invalidated inside it, not outside.
        EntityList::BeginTick();

        // Did the ground move? Has to come before anything reads an
        // entity, so nothing this tick works from last world's data.
        UpdateLifecycle(env);
        ClearJavaException(env);

        // Camera before the modules: the overlays should match the
        // frame the game is about to draw.
        Camera::Update(env);
        ClearJavaException(env);

        // One read of "what happened this tick", shared by every
        // module.
        CombatState::Update(env);
        ClearJavaException(env);

        // ---- Cursor ----
        // The menu is useless without this. A grabbed mouse is
        // pinned to the centre of the window and its movement is fed
        // to the camera, so the pointer can never reach a control.
        MouseControl::Apply(env, s_menuOpen.load());
        ClearJavaException(env);

        // ---- Work handed over by the UI ----
        {
            std::vector<std::function<void(JNIEnv*)>> pending;
            {
                std::lock_guard<std::mutex> lock(s_actionMutex);
                pending.swap(s_actions);
            }
            for (auto& fn : pending) {
                try {
                    fn(env);
                } catch (...) {
                    iOS::Notify::Error("An action failed",
                        "Something the menu asked for could not be applied.");
                }
                ClearJavaException(env);
            }
        }

        // ---- Keybinds, only while the game window is focused ----
        bool focused = (s_gameWindow == nullptr)
                    || (GetForegroundWindow() == s_gameWindow);
        bool hotkeys = focused && !s_menuOpen.load() && !KeyCapture::IsActive();

        if (hotkeys && !s_hotkeysWere) {
            for (auto& mod : s_modules) {
                int key = mod->GetKeybind();
                if (key <= 0 || key > 255) continue;
                s_keyStates[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
            }
        }
        s_hotkeysWere = hotkeys;

        for (auto& mod : s_modules) {
            int key = mod->GetKeybind();
            if (key <= 0 || key > 255) continue;

            bool pressed = hotkeys && ((GetAsyncKeyState(key) & 0x8000) != 0);
            if (pressed && !s_keyStates[key]) {
                mod->Toggle(env);
                ClearJavaException(env);
            }
            s_keyStates[key] = pressed;
        }

        // ---- Run enabled modules ----
        // While the Phantom menu is open the mouse is the UI's, not
        // the game's, so gameplay stands down. Visual modules (ESP,
        // Fullbright) are pure rendering state and stay live. The
        // open EDGE lets go of everything held for the player, once.
        bool menuOpen = s_menuOpen.load();

        if (menuOpen && !s_gameplayHalted) {
            KeyBinds::ReleaseAll(env);
            ClearJavaException(env);
            s_gameplayHalted = true;
        } else if (!menuOpen) {
            s_gameplayHalted = false;
        }

        for (auto& mod : s_modules) {
            if (!mod->IsEnabled()) continue;
            if (menuOpen && mod->GetCategory() != ModuleCategory::VISUAL)
                continue;
            RunModule(env, mod);
        }

        // ---- Key reconciliation ----
        // pressed is edge driven, so a key we cleared while the
        // player was holding it never came back on its own. Anything
        // we are no longer driving gets put back in line with the
        // hardware here. Still runs while the menu is up, so a key
        // the player is physically holding stays correct even with
        // gameplay halted. Skipped inside a vanilla GUI, where the
        // game clears every key on purpose and we would fight it.
        if (!Minecraft::IsInGui(env)) {
            KeyBinds::Reconcile(env);
            ClearJavaException(env);
        }

        // Drops every entity ref the cache was holding
        EntityList::BeginTick();
        env->PopLocalFrame(nullptr);
    }

    static void Shutdown(JNIEnv* env) {
        Camera::Invalidate();
        CombatState::Reset();
        Rotation::ResetVelocity();

        if (env) {
            // Never eject with the cursor loose: the player would be
            // left unable to look around.
            MouseControl::ForceRestore(env);
            ClearJavaException(env);

            // Disable everything so modules release the keybinds they
            // took over instead of leaving keys stuck down.
            for (auto& mod : s_modules) {
                if (!mod->IsEnabled()) continue;
                try {
                    mod->SetEnabled(false, env);
                } catch (...) {}
                ClearJavaException(env);
            }

            KeyBinds::ReleaseAll(env);
            ClearJavaException(env);

            if (s_lastWorld) {
                env->DeleteGlobalRef(s_lastWorld);
                s_lastWorld = nullptr;
            }
        }
        MouseControl::Shutdown();
        {
            std::lock_guard<std::mutex> lock(s_actionMutex);
            s_actions.clear();
        }
        s_faults.clear();
        s_esp.reset();
        s_modules.clear();
    }

    static int GetModuleCount() { return (int)s_modules.size(); }
    static std::vector<std::shared_ptr<Module>>& GetModules() { return s_modules; }

    // -------------------------------------------------------------
    // Category lists are read once per frame per tab by the menu.
    // -------------------------------------------------------------
    static const std::vector<std::shared_ptr<Module>>&
    GetModulesByCategory(ModuleCategory cat) {
        static std::unordered_map<int, std::vector<std::shared_ptr<Module>>> cache;
        static size_t builtFor = (size_t)-1;

        if (builtFor != s_modules.size()) {
            cache.clear();
            for (auto& m : s_modules)
                cache[(int)m->GetCategory()].push_back(m);
            builtFor = s_modules.size();
        }
        return cache[(int)cat];
    }
};
