#include <Windows.h>
#include <thread>
#include <cstdio>

#include "jni/jni_helper.h"
#include "jni/class_resolver.h"
#include "hooks/gl_hook.h"
#include "mc/minecraft.h"
#include "mc/entity_list.h"
#include "modules/module_manager.h"

FILE* g_console = nullptr;

void MainThread(HMODULE hModule) {
    AllocConsole();
    freopen_s(&g_console, "CONOUT$", "w", stdout);
    printf("[Phantom] Injected into Lunar Client 1.8.9\n");

    // 1. Attach to JVM
    if (!JNIHelper::Initialize()) {
        printf("[Phantom] FATAL: Could not attach to JVM\n");
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    printf("[Phantom] JVM attached (version 0x%x)\n", JNIHelper::GetVersion());

    // 2. Resolve Minecraft classes via JVMTI
    if (!ClassResolver::ResolveAll(JNIHelper::GetEnv())) {
        printf("[Phantom] FATAL: Could not resolve MC classes\n");
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    printf("[Phantom] MC classes resolved\n");

    // 3. Init Minecraft wrapper
    Minecraft::Init(JNIHelper::GetEnv());
    printf("[Phantom] Minecraft instance acquired\n");

    // 4. Init EntityList (may fail if not in-game yet, will retry later)
    if (EntityList::Init(JNIHelper::GetEnv())) {
        printf("[Phantom] EntityList initialized\n");
    } else {
        printf("[Phantom] EntityList deferred (not in-game yet)\n");
    }

    // 5. Register modules
    ModuleManager::Init();
    printf("[Phantom] %d modules registered\n", ModuleManager::GetModuleCount());

    // 6. Hook wglSwapBuffers for ImGui overlay
    if (!GLHook::Install()) {
        printf("[Phantom] FATAL: Could not hook wglSwapBuffers\n");
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }
    printf("[Phantom] OpenGL hooked. Press INSERT to open menu.\n");
    printf("[Phantom] Press DELETE to eject.\n");

    // Main loop
    while (!GetAsyncKeyState(VK_DELETE)) {
        // Tick enabled modules
        ModuleManager::Tick(JNIHelper::GetEnv());
        Sleep(1);
    }

    // Cleanup
    printf("[Phantom] Ejecting...\n");
    GLHook::Remove();
    ModuleManager::Shutdown();
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
