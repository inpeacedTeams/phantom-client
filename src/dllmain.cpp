#include <Windows.h>
#include <thread>
#include <chrono>
#include <cstdio>

#include "jni/jni_helper.h"
#include "jni/class_resolver.h"
#include "hooks/gl_hook.h"
#include "gui/splash.h"
#include "gui/notifications.h"
#include "mc/minecraft.h"
#include "mc/entity_list.h"
#include "mc/keybinds.h"
#include "modules/module_manager.h"
#include "config/config_store.h"

// =================================================================
// Entry point
// =================================================================
// THE CONSOLE
// A debug console behind a fullscreen game is not a user interface.
// Everything the player needs to know now arrives as an on-screen
// notification, so the console only exists in a debug build, where
// it is genuinely useful for reading resolution failures.
//
// PHANTOM_CONSOLE can be defined to force it on in a release build
// when something needs diagnosing on someone else's machine.
// =================================================================

#if defined(_DEBUG) || defined(PHANTOM_CONSOLE)
  #define PHANTOM_WANT_CONSOLE 1
#else
  #define PHANTOM_WANT_CONSOLE 0
#endif

static FILE* g_console = nullptr;

// Minecraft runs at 20 ticks per second. Every module expresses its
// delays in ticks, so the client loop has to match. Running faster
// made a "2 tick" delay expire in 2 milliseconds.
//
// Click timing does NOT live here: the autoclicker runs on its own
// 1ms thread, because a 50ms loop cannot express a 27ms gap.
static constexpr int TICK_MS = 50;

// The config is written on a timer as well as at eject. Crashing
// out of a game, or killing the process, should not cost you the
// last hour of tuning.
static constexpr int AUTOSAVE_TICKS = 20 * 60;   // one minute

static void OpenConsole() {
#if PHANTOM_WANT_CONSOLE
    AllocConsole();
    freopen_s(&g_console, "CONOUT$", "w", stdout);
    SetConsoleTitleA("Phantom");
#endif
}

static void CloseConsole() {
#if PHANTOM_WANT_CONSOLE
    if (g_console) fclose(g_console);
    FreeConsole();
#endif
}

static bool WaitForResolve(JNIEnv* env, int maxSeconds) {
    const int attempts = (maxSeconds * 1000) / 500;
    for (int i = 0; i < attempts; i++) {
        if (ClassResolver::ResolveAll(env) && Minecraft::Init(env))
            return true;
        if (i == 0)
            printf("[Phantom] Waiting for the game to finish loading...\n");
        Sleep(500);
    }
    return false;
}

void MainThread(HMODULE hModule) {
    OpenConsole();
    printf("[Phantom] Injected into Lunar Client 1.8.9\n");

    // ---- 1. Attach to the running JVM ----
    if (!JNIHelper::Initialize()) {
        printf("[Phantom] FATAL: could not attach to the JVM\n");
        // No overlay yet, so a message box is the only way to say
        // anything at all. It is the one place one is justified.
        MessageBoxA(nullptr,
            "Phantom could not attach to the game's Java VM.\n\n"
            "Make sure you injected into the Minecraft process itself "
            "rather than the launcher.",
            "Phantom", MB_ICONERROR | MB_OK);
        CloseConsole();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    JNIEnv* env = JNIHelper::GetEnv();
    printf("[Phantom] JVM attached (JNI 0x%x)\n", JNIHelper::GetVersion());

    // ---- 2. Resolve classes and the Minecraft singleton ----
    // Injecting at the main menu is normal, so retry rather than
    // giving up on the first pass.
    if (!WaitForResolve(env, 60)) {
        printf("[Phantom] FATAL: could not resolve Minecraft classes\n");
#if PHANTOM_WANT_CONSOLE
        printf("[Phantom] Dumping loaded classes for diagnosis:\n");
        ClassResolver::DumpAllClasses(env);
#endif
        MessageBoxA(nullptr,
            "Phantom could not find Minecraft's classes.\n\n"
            "This build targets Lunar Client 1.8.9. A different version "
            "or a heavily modified client will not be recognised.",
            "Phantom", MB_ICONERROR | MB_OK);
        JNIHelper::Detach();
        CloseConsole();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    printf("[Phantom] Minecraft resolved\n");

    // ---- 3. Keybinds ----
    // Most modules act by holding real keys, so without these they
    // are inert. GameSettings exists at the main menu, so this
    // normally succeeds immediately.
    if (KeyBinds::Init(env)) printf("[Phantom] Keybinds resolved\n");
    else printf("[Phantom] Keybinds deferred, will retry in the loop\n");

    // ---- 4. Modules ----
    ModuleManager::Init();
    printf("[Phantom] %d modules registered\n", ModuleManager::GetModuleCount());

    // ---- 5. Restore the last session ----
    // Before the overlay exists, so the very first frame already
    // shows the user's own accent, scale and module list rather
    // than defaults that then jump.
    bool hadConfig = ConfigStore::Exists("default");
    if (hadConfig) {
        ConfigStore::Report r = ConfigStore::Load("default", env);
        printf("[Phantom] %s\n", r.message.c_str());
    } else {
        printf("[Phantom] No saved config, starting from defaults\n");
    }

    // ---- 6. Overlay ----
    if (!GLHook::Install()) {
        printf("[Phantom] FATAL: could not hook wglSwapBuffers\n");
        MessageBoxA(nullptr,
            "Phantom could not hook the game's renderer.\n\n"
            "Another overlay may already have it, or the game may be "
            "running on a renderer this build does not support.",
            "Phantom", MB_ICONERROR | MB_OK);
        ModuleManager::Shutdown(env);
        JNIHelper::Detach();
        CloseConsole();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    // ---- 7. Say hello ----
    // Only now, because everything above can still fail. A console
    // window behind the game is not confirmation: you cannot see it.
    // This is the first and only sign on screen that the client
    // loaded, so it is armed at the point where that is true.
    //
    // Trigger only raises a flag. The render thread starts the clock
    // on its next frame, once the fonts exist.
    iOS::Splash::Trigger();

    if (hadConfig) {
        iOS::Notify::Success("Config restored",
                             ConfigStore::LastReport().message);
    }
    if (!KeyBinds::HasClickQueue()) {
        iOS::Notify::Warning("Clicking unavailable",
            "The game's click counter could not be found, so AutoClicker "
            "and Hit Select cannot send clicks.");
    }

    printf("[Phantom] Ready. INSERT opens the menu, DELETE ejects.\n");

    // ---- 8. Client loop ----
    auto next = std::chrono::steady_clock::now();
    bool ejectHeld = false;
    int autosave = AUTOSAVE_TICKS;

    while (true) {
        bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (del && !ejectHeld) break;
        ejectHeld = del;

        if (Minecraft::InGame(env)) {
            KeyBinds::Init(env);     // no-op once resolved
            EntityList::Init(env);   // needs a loaded world
            ModuleManager::Tick(env);
        } else {
            // Left the world. Drop any key a module was holding,
            // otherwise it follows you into the next game.
            ModuleManager::OnLeaveWorld(env);
        }

        // Periodic save. Quiet: a toast every minute would be noise.
        if (--autosave <= 0) {
            autosave = AUTOSAVE_TICKS;
            ConfigStore::Save("default");
        }

        next += std::chrono::milliseconds(TICK_MS);
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;   // we fell behind, resync instead of spinning
    }

    // ---- 9. Teardown ----
    // Order matters. The config is written FIRST, while every module
    // is still alive and holding its settings. GLHook::Remove then
    // stops the render thread from calling into module state, so it
    // has to happen before the modules are destroyed.
    printf("[Phantom] Ejecting...\n");

    ConfigStore::Save("default");

    GLHook::Remove();
    ModuleManager::Shutdown(env);
    KeyBinds::Shutdown(env);
    EntityList::Shutdown(env);
    Minecraft::Shutdown(env);
    JNIHelper::Detach();

    CloseConsole();
    FreeLibraryAndExitThread(hModule, 0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        std::thread(MainThread, hModule).detach();
    }
    return TRUE;
}
