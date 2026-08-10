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
#include "../input/click_scheduler.h"
#include "../input/key_capture.h"
#include "../render/camera.h"
#include "../gui/notifications.h"

// Combat
#include "combat/aim_assist.h"
#include "combat/kill_aura.h"
#include "combat/velocity.h"
#include "combat/sprint_reset.h"
#include "combat/autoblockhit.h"
#include "combat/hitselect.h"
#include "combat/backtrack.h"
#include "combat/autoclicker.h"

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
// pushed onto the action queue and executed here instead. That
// includes opening the menu itself: releasing the mouse grab is a
// call into Minecraft, so the window thread only sets a flag and
// this tick acts on it.
//
// The click timer runs on a third thread. It never touches JNI or
// module state either: it watches the mouse button and increments a
// counter, which this tick drains and hands to the game.
//
// Every tick runs inside a JNI local frame. Without it the local
// reference table fills up within seconds and the JVM aborts.
//
// TICK ORDER  (all of it is load-bearing)
//
//   1. Push the local frame, invalidate the entity cache.
//   2. Lifecycle: has the world changed under us, did we just die?
//   3. Backtrack restores real positions. Everything downstream,
//      including the ESP, must see the world as the server has it.
//   4. Camera snapshot, taken before any module rotates the player.
//   5. CombatState, so every module reads the same facts about this
//      tick rather than each deriving its own from the mouse.
//   6. Mouse grab follows the menu.
//   7. UI actions, keybinds, then the modules themselves. While the
//      menu is open the gameplay modules are held down, because the
//      cursor is the UI's and their input would be aimed at it.
//   8. Queued clicks are handed to the game LAST, so a click fired
//      by a module this tick goes out on this tick.
//   9. Key reconciliation, after everything has had its say.
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
// exists: a target that despawned, a timer counting toward a fight
// that ended, recorded positions from another world.
//
// Rather than have fifteen modules each try to notice, the manager
// watches for it once and calls OnReset on all of them.
//
// FAULT ISOLATION
// A module that throws used to have its stack trace printed to a
// console behind the game, and then be called again next tick, and
// the tick after that, forever. Twenty times a second of exception
// handling is a visible frame cost and the user is told nothing.
//
// Faults are now counted per module. A few in a row means the
// module is broken rather than unlucky, so it is switched off and
// the user gets a notification saying which one and why. The rest
// of the client keeps running.
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
    inline static std::shared_ptr<Backtrack> s_backtrack;
    inline static std::shared_ptr<AutoClicker> s_autoClicker;
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
    // stand down: one that kept moving the camera or firing clicks
    // would be acting on input the player is aiming at a switch. The
    // edge is where held keys and any queued clicks are let go, once.
    inline static bool s_gameplayHalted = false;

    // A packed lobby can hold 60+ players, and the entity scan takes
    // a couple of refs each on top of whatever the modules allocate.
    // 256 was close enough to the edge to matter.
    static constexpr jint kLocalFrameSize = 768;

    // -------------------------------------------------------------
    // Swallow a pending Java exception and say whether there was one.
    //
    // ExceptionDescribe is deliberately not called: it writes a
    // stack trace to a console the user cannot see, and it is slow
    // enough to matter if something throws every tick.
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

        // A module reaching a bad JNI object throws C++ side; a
        // module calling into Minecraft badly throws Java side. Both
        // have to be caught or the client dies with the game.
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
            // A clean tick clears the record. Only CONSECUTIVE
            // failures mean the module is actually broken.
            auto it = s_faults.find(name);
            if (it != s_faults.end()) s_faults.erase(it);
            return;
        }

        int count = ++s_faults[name];
        if (count < kFaultLimit) return;

        // Persistently broken. Turn it off, release whatever it was
        // holding, and tell the user which one and why.
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
        // Shared state first, so a module's OnReset reads a world
        // that has already been cleared rather than a half-old one.
        CombatState::Reset();
        Rotation::ResetVelocity();
        Camera::Invalidate();
        EntityList::BeginTick();
        ClickScheduler::ClearPending();
        s_faults.clear();

        for (auto& mod : s_modules) {
            try {
                mod->OnReset(env);
            } catch (...) {}
            ClearJavaException(env);
        }

        // Whatever was being held was being held for a situation
        // that no longer exists.
        KeyBinds::ReleaseAll(env);
        KeyBinds::ClearClickQueue(env);
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
            // Between worlds. Drop the ref now so coming back into a
            // world always counts as a change, even in the unlikely
            // case the JVM hands us the same address again.
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

        // Death and respawn both matter. Dying mid-combo leaves
        // every combat module mid-combo; respawning puts you
        // somewhere else entirely.
        jobject player = Minecraft::GetPlayer(env);
        ClearJavaException(env);

        bool dead = player && Minecraft::IsDead(env, player);
        if (dead != s_wasDead) {
            s_wasDead = dead;
            changed = true;
        }

        if (changed) BroadcastReset(env);
    }

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

        if (done > 0 && s_autoClicker) s_autoClicker->NoteDelivered(done);
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

        s_autoClicker = std::make_shared<AutoClicker>();
        s_modules.push_back(s_autoClicker);

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

        // The click timer runs on its own 1ms thread and watches the
        // mouse itself; the 20 TPS loop can express neither a 27ms
        // gap nor a prompt release.
        ClickScheduler::Start();
    }

    static void SetGameWindow(HWND hwnd) {
        s_gameWindow = hwnd;
        ClickScheduler::SetWindow(hwnd);
    }

    // Called from the window thread when INSERT is pressed. Does no
    // JNI work itself: releasing the grab is a JNI call and belongs
    // on the client thread, which reads s_menuOpen next tick.
    //
    // The click engine, though, polls the mouse on its own thread
    // and cannot wait for a tick to learn the menu opened, so its
    // flag is pushed straight through here. That is safe off-thread:
    // it only touches an atomic.
    static void SetMenuOpen(bool open) {
        s_menuOpen.store(open);
        ClickScheduler::SetMenuOpen(open);
    }
    static bool IsMenuOpen()           { return s_menuOpen.load(); }

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

        ClickScheduler::SetArmed(false);
        ClickScheduler::ClearPending();
        Camera::Invalidate();       // overlays stop drawing stale geometry
        CombatState::Reset();       // swing history from the last server is meaningless
        Rotation::ResetVelocity();  // or the arm keeps drifting in the next game
        MouseControl::Shutdown();   // the next world re-resolves and re-grabs
        s_faults.clear();           // a world change is not a module fault
        s_wasDead = false;
        s_gameplayHalted = false;   // re-entry re-evaluates the menu edge cleanly

        if (env) {
            // Modules are still enabled and will run again the
            // moment the next world loads, so they get the same
            // clean slate a mid-game world change would give them.
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
            KeyBinds::ClearClickQueue(env);
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

        // Undo last tick's rewind before anything reads an entity.
        // Runs even while the module is off, so a toggle mid-rewind
        // still leaves the world in the right place.
        if (s_backtrack) {
            s_backtrack->RestoreBeforeScan(env);
            ClearJavaException(env);
        }

        // Camera before the modules: aim assist rotates the player
        // later in the tick, and the overlays should match the frame
        // the game is about to draw.
        Camera::Update(env);
        ClearJavaException(env);

        // One read of "what happened this tick", shared by every
        // module. Before this existed each of them polled the mouse
        // button and called that an attack.
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
        // Suppressed while the menu is open, or typing near a bound
        // letter toggles things behind the UI. Suppressed again
        // while a bind is being captured: the whole point of that
        // moment is that the next key means something else.
        bool focused = (s_gameWindow == nullptr)
                    || (GetForegroundWindow() == s_gameWindow);
        bool hotkeys = focused && !s_menuOpen.load() && !KeyCapture::IsActive();

        // Coming back from a suppressed spell, adopt the current
        // hardware state without acting on it. Otherwise a key the
        // player never let go of reads as a brand new press the
        // instant the menu closes, and a module toggles itself.
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
        // the game's. A combat or movement module ticking then acts
        // on input the player is aiming at a control: it whips the
        // crosshair around or fires an attack behind the menu. So
        // gameplay stands down while it is open. Visual modules (ESP,
        // Fullbright) are pure rendering state and stay live, so
        // their boxes and brightness do not blink in the menu.
        //
        // The open EDGE lets go of everything held for the player,
        // once. Modules stay enabled and resume the moment it
        // closes; this is the same clean slate a world change hands
        // them, not a disable.
        bool menuOpen = s_menuOpen.load();

        if (menuOpen && !s_gameplayHalted) {
            KeyBinds::ReleaseAll(env);
            KeyBinds::ClearClickQueue(env);
            ClickScheduler::ClearPending();
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

        // ---- Clicks, last, so this tick's requests go out now ----
        // Skipped while the menu is open: the engine is already muted
        // on the same edge, but draining into the game here would
        // still be wrong.
        if (!menuOpen) {
            DispatchClicks(env);
            ClearJavaException(env);
        }

        // ---- Key reconciliation ----
        // The reason a sprint reset could freeze you: pressed is
        // edge driven, so a key we cleared while the player was
        // holding it never came back on its own. Anything we are no
        // longer driving gets put back in line with the hardware
        // here, so a missed release costs one tick instead of the
        // rest of the fight. This still runs while the menu is up, so
        // a key the player is physically holding stays correct even
        // with gameplay halted.
        //
        // Skipped inside a GUI, where the game clears every key on
        // purpose and we would be fighting it.
        if (!Minecraft::IsInGui(env)) {
            KeyBinds::Reconcile(env);
            ClearJavaException(env);
        }

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
            // Never eject with the cursor loose: the player would be
            // left unable to look around.
            MouseControl::ForceRestore(env);
            ClearJavaException(env);

            // Disable everything so modules release the keybinds they
            // took over instead of leaving keys stuck down. One that
            // throws on the way out must not stop the others.
            for (auto& mod : s_modules) {
                if (!mod->IsEnabled()) continue;
                try {
                    mod->SetEnabled(false, env);
                } catch (...) {}
                ClearJavaException(env);
            }

            // Backstop: a module that threw during OnDisable would
            // still have left something held.
            KeyBinds::ReleaseAll(env);
            KeyBinds::ClearClickQueue(env);
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
        s_backtrack.reset();
        s_esp.reset();
        s_autoClicker.reset();
        s_modules.clear();
    }

    static int GetModuleCount() { return (int)s_modules.size(); }
    static std::vector<std::shared_ptr<Module>>& GetModules() { return s_modules; }

    // -------------------------------------------------------------
    // Category lists are read once per frame per tab by the menu.
    // Building a fresh vector for each of those was a few hundred
    // allocations a second to produce a list that only changes when
    // the module set does, which is never after startup.
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
