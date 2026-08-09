#include <Windows.h>
#include <thread>
#include <chrono>
#include <cstdio>

#include "jni/jni_helper.h"
#include "jni/class_resolver.h"
#include "hooks/gl_hook.h"
#include "mc/minecraft.h"
#include "mc/keybinds.h"
#include "mc/entity_list.h"
#include "modules/module_manager.h"

FILE* g_console = nullptr;

// Minecraft runs at 20 ticks per second and every module expresses
// its delays in ticks, so the client loop has to match. An earlier
// version used Sleep(1), which ran roughly 1000 iterations a second
// and made every "2 tick" delay expire in two milliseconds.
static constexpr int TICK_MS = 50;

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
    AllocConsole();
    freopen_s(&g_console, "CONOUT$", "w", stdout);
    printf("[Phantom] Injected into Lunar Client 1.8.9\n");

    // ---- 1. Attach to the running JVM ----
    if (!JNIHelper::Initialize()) {
        printf("[Phantom] FATAL: could not attach to the JVM\n");
        Sleep(4000);
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    JNIEnv* env = JNIHelper::GetEnv();
    printf("[Phantom] JVM attached (JNI 0x%x)\n", JNIHelper::GetVersion());

    // ---- 2. Resolve classes and the Minecraft singleton ----
    // Injecting at the main menu is normal, so retry instead of
    // giving up on the first pass.
    if (!WaitForResolve(env, 60)) {
        printf("[Phantom] FATAL: could not resolve Minecraft classes\n");
        printf("[Phantom] Dumping loaded classes for diagnosis:\n");
        ClassResolver::DumpAllClasses(env);
        Sleep(6000);
        JNIHelper::Detach();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    printf("[Phantom] Minecraft resolved\n");

    // ---- 3. Keybinds ----
    // Most modules simulate input by driving these, so without them
    // the client is inert. Not fatal: they resolve later if the
    // options object is not ready yet.
    if (KeyBinds::Init(env)) printf("[Phantom] Keybinds resolved\n");
    else                     printf("[Phantom] Keybinds deferred\n");

    // ---- 4. Modules ----
    ModuleManager::Init();
    printf("[Phantom] %d modules registered\n", ModuleManager::GetModuleCount());

    // ---- 5. Overlay ----
    if (!GLHook::Install()) {
        printf("[Phantom] FATAL: could not hook wglSwapBuffers\n");
        Sleep(4000);
        ModuleManager::Shutdown(env);
        JNIHelper::Detach();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    printf("[Phantom] Ready. INSERT opens the menu, DELETE ejects.\n");

    // ---- 6. Client loop ----
    auto next = std::chrono::steady_clock::now();
    bool ejectHeld = false;

    while (true) {
        bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (del && !ejectHeld) break;
        ejectHeld = del;

        if (Minecraft::InGame(env)) {
            // Both retry quietly until a world exists
            KeyBinds::Init(env);
            EntityList::Init(env);
            ModuleManager::Tick(env);
        }

        next += std::chrono::milliseconds(TICK_MS);
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;   // fell behind, resync rather than spin
    }

    // ---- 7. Teardown ----
    printf("[Phantom] Ejecting...\n");
    GLHook::Remove();

    // Disables every module first, so anything holding a keybind
    // lets go before we drop the references.
    ModuleManager::Shutdown(env);
    KeyBinds::Shutdown(env);
    EntityList::Shutdown(env);
    Minecraft::Shutdown(env);
    JNIHelper::Detach();

    if (g_console) fclose(g_console);
    FreeConsole();
    FreeLibraryAndExitThread(hModule, 0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        std::thread(MainThread, hModule).detach();
    }
    return TRUE;
}
