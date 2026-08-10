#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <Windows.h>
#include "module.h"
#include "../mc/keybinds.h"
#include "../mc/entity_list.h"
#include "../mc/combat_state.h"
#include "../mc/rotation.h"
#include "../input/click_scheduler.h"
#include "../render/camera.h"

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
// Tick() runs on our client thread, which is attached to the JVM.
// It is the only place JNI may be touched.
//
// The render thread draws the menu and reads module state but never
// calls JNI. Anything the UI wants to do that needs a JNIEnv is
// pushed onto the action queue and executed here instead.
//
// The click timer runs on a third thread. It never touches JNI or
// module state either: it only increments a counter, which this
// tick drains and hands to the game.
//
// Every tick runs inside a JNI local frame. Without it the local
// reference table fills up within seconds and the JVM aborts.
//
// TICK ORDER  (all of it is load-bearing)
//
//   1. Push the local frame, invalidate the entity cache.
//   2. Backtrack restores real positions. Everything downstream,
//      including the ESP, must see the world as the server has it.
//   3. Camera snapshot, taken before any module rotates the player.
//   4. CombatState, so every module reads the same facts about this
//      tick rather than each deriving its own from the mouse.
//   5. UI actions, keybinds, then the modules themselves.
//   6. Queued clicks are handed to the game LAST, so a click fired
//      by a module this tick goes out on this tick.
//
// KEY SAFETY
// Modules hold real keybinds down. If the world goes away or we
// eject while a key is held, the player is left walking into a wall
// with no way to stop, so every exit path releases them.
// =================================================================

class ModuleManager {
private:
    inline static std::vector<std::shared_ptr<Module>> s_modules;
    inline static bool s_keyStates[256] = {};

    inline static std::vector<std::function<void(JNIEnv*)>> s_actions;
    inline static std::mutex s_actionMutex;

    inline static HWND s_gameWindow = nullptr;
    inline static std::shared_ptr<ESP> s_esp;
    inline static std::shared_ptr<Backtrack> s_backtrack;
    inline static std::shared_ptr<ClickAssist> s_clickAssist;
    inline static bool s_wasInGame = false;

    // A packed lobby can hold 60+ players, and the entity scan takes
    // a couple of refs each on top of whatever the modules allocate.
    // 256 was close enough to the edge to matter.
    static constexpr jint kLocalFrameSize = 768;

    // -------------------------------------------------------------
    // Hand the game whatever the click timer produced.
    //
    // This is the only place clicks reach Minecraft. Writing to
    // KeyBinding.pressTime makes runTick call clickMouse() exactly
    // that many times, which is the same path a physical click
    // takes, so the swing and the attack packet are identical.
    // -------------------------------------------------------------
    static void DispatchClicks(JNIEnv* env) {
        if (!KeyBinds::HasClickQueue()) return;

        int left  = ClickScheduler::DrainLeft();
        int right = ClickScheduler::DrainRight();
        if (left <= 0 && right <= 0) return;

        // A screen is open: runTick stops consuming pressTime, so
        // anything written now would sit there and then fire all at
        // once when the menu closes.
        if (Minecraft::IsInGui(env)) return;

        int done = 0;
        if (left  > 0) done += KeyBinds::QueueAttack(env, left);
        if (right > 0) done += KeyBinds::QueueUse(env, right);

        if (done > 0 && s_clickAssist) s_clickAssist->NoteDelivered(done);
    }

public:
    static void Init() {
        // Combat, core first so they sit at the top of the tab
        s_modules.push_back(std::make_shared<Velocity>());
        s_modules.push_back(std::make_shared<SprintReset>());
        s_modules.push_back(std::make_shared<AutoBlockhit>());
        s_modules.push_back(std::make_shared<AimAssist>());
        s_modules.push_back(std::make_shared<HitSelect>());

        s_backtrack = std::make_shared<Backtrack>();
        s_modules.push_back(s_backtrack);

        s_clickAssist = std::make_shared<ClickAssist>();
        s_modules.push_back(s_clickAssist);

        s_modules.push_back(std::make_shared<KillAura>());

        // Movement
        s_modules.push_back(std::make_shared<BridgeAssist>());
        s_modules.push_back(std::make_shared<Sprint>());
        s_modules.push_back(std::make_shared<NoJumpDelay>());
        s_modules.push_back(std::make_shared<Speed>());
        s_modules.push_back(std::make_shared<Fly>());

        // Visual
        s_esp = std::make_shared<ESP>();
        s_modules.push_back(s_esp);
        s_modules.push_back(std::make_shared<Fullbright>());

        // Click timing runs on its own 1ms-resolution thread; the
        // 20 TPS loop cannot express a 27ms gap.
        ClickScheduler::Start();
    }

    static void SetGameWindow(HWND hwnd) {
        s_gameWindow = hwnd;
        ClickScheduler::SetWindow(hwnd);
    }

    static std::shared_ptr<ESP> GetESP() { return s_esp; }
    static std::shared_ptr<Backtrack> GetBacktrack() { return s_backtrack; }

    template <typename T>
    static std::shared_ptr<T> Get() {
        for (auto& m : s_modules) {
            auto casted = std::dynamic_pointer_cast<T>(m);
            if (casted) return casted;
        }
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

        ClickScheduler::SetActive(false);
        ClickScheduler::ClearPending();
        Camera::Invalidate();       // overlays stop drawing stale geometry
        CombatState::Reset();       // swing history from the last server is meaningless
        Rotation::ResetVelocity();  // or the arm keeps drifting in the next game

        if (env) {
            KeyBinds::ReleaseAll(env);
            KeyBinds::ClearClickQueue(env);
        }
    }

    static void Tick(JNIEnv* env) {
        if (!env) return;
        s_wasInGame = true;

        if (env->PushLocalFrame(kLocalFrameSize) != 0) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return;
        }

        // Entity refs belong to the frame we just pushed, so the
        // cache has to be invalidated inside it, not outside.
        EntityList::BeginTick();

        // Undo last tick's rewind before anything reads an entity.
        // Runs even while the module is off, so a toggle mid-rewind
        // still leaves the world in the right place.
        if (s_backtrack) {
            s_backtrack->RestoreBeforeScan(env);
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        }

        // Camera before the modules: aim assist rotates the player
        // later in the tick, and the overlays should match the frame
        // the game is about to draw.
        Camera::Update(env);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }

        // One read of "what happened this tick", shared by every
        // module. Before this existed each of them polled the mouse
        // button and called that an attack.
        CombatState::Update(env);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }

        // ---- Work handed over by the UI ----
        {
            std::vector<std::function<void(JNIEnv*)>> pending;
            {
                std::lock_guard<std::mutex> lock(s_actionMutex);
                pending.swap(s_actions);
            }
            for (auto& fn : pending) {
                fn(env);
                if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
            }
        }

        // ---- Keybinds, only while the game window is focused ----
        bool focused = (s_gameWindow == nullptr)
                    || (GetForegroundWindow() == s_gameWindow);

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

        // ---- Clicks, last, so this tick's requests go out now ----
        DispatchClicks(env);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }

        // Drops every entity ref the cache was holding
        EntityList::BeginTick();
        env->PopLocalFrame(nullptr);
    }

    static void Shutdown(JNIEnv* env) {
        // Stop the click thread first so nothing is queued while the
        // modules are being torn down.
        ClickScheduler::Stop();
        Camera::Invalidate();
        CombatState::Reset();
        Rotation::ResetVelocity();

        if (env) {
            // Disable everything so modules release the keybinds they
            // took over instead of leaving keys stuck down.
            for (auto& mod : s_modules)
                if (mod->IsEnabled()) mod->SetEnabled(false, env);

            // Backstop: a module that threw during OnDisable would
            // still have left something held.
            KeyBinds::ReleaseAll(env);
            KeyBinds::ClearClickQueue(env);
        }
        {
            std::lock_guard<std::mutex> lock(s_actionMutex);
            s_actions.clear();
        }
        s_backtrack.reset();
        s_esp.reset();
        s_clickAssist.reset();
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
