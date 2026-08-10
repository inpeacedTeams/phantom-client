#pragma once
#include <Windows.h>
#include <gl/GL.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>
#include <atomic>
#include <cstdio>

#include "../gui/menu.h"
#include "../mc/mouse_control.h"
#include "../modules/module_manager.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef BOOL(WINAPI* fnWglSwapBuffers)(HDC);

// =================================================================
// GLHook
// =================================================================
// Hooks wglSwapBuffers, creates a second GL context that shares the
// game's objects, draws ImGui into it, then restores the original
// context before letting the real swap through.
//
// THE CURSOR
// Opening the menu is not just a draw flag. While you are in a
// world Minecraft GRABS the mouse: the pointer is locked to the
// centre and its movement is fed to the camera, so no amount of
// cursor drawing lets you reach a switch.
//
// Releasing that grab is a call into Minecraft, and JNI is only
// legal on the client thread, so this file does not do it. It sets
// a flag and ModuleManager acts on it next tick, 50ms at worst.
//
// EJECT IS THE DANGEROUS PART
// The render thread runs this hook while the client thread tears
// everything down. A frame still inside Menu::Render() when the
// module list is freed walks dead memory and crashes the game, so
// there is a shutdown flag plus an in-flight counter: Remove()
// raises the flag, waits for the counter to reach zero, and only
// then destroys anything.
// =================================================================

class GLHook {
private:
    inline static fnWglSwapBuffers oSwapBuffers = nullptr;
    inline static WNDPROC oWndProc     = nullptr;
    inline static HWND    gameWindow   = nullptr;
    inline static HGLRC   imguiContext = nullptr;
    inline static void*   pSwapBuffers = nullptr;

    inline static std::atomic<bool> s_ready{ false };
    inline static std::atomic<bool> s_shuttingDown{ false };
    inline static std::atomic<int>  s_inFlight{ 0 };
    inline static std::atomic<bool> s_menuOpen{ false };

    // One place that changes menu state, so the mouse grab can never
    // fall out of step with what is on screen.
    static void ApplyMenuState(bool open) {
        s_menuOpen.store(open);
        ModuleManager::SetMenuOpen(open);
    }

    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s_shuttingDown.load() && msg == WM_KEYDOWN) {
            if (wParam == VK_INSERT) {
                ApplyMenuState(!s_menuOpen.load());
                return 0;
            }
            // ESC closes the menu rather than falling through to the
            // game, where it would open the pause screen underneath.
            if (wParam == VK_ESCAPE && s_menuOpen.load()) {
                ApplyMenuState(false);
                return 0;
            }
        }

        // The window losing focus with the menu open would leave the
        // grab released and the player unable to look around.
        if (msg == WM_KILLFOCUS && s_menuOpen.load()) {
            ApplyMenuState(false);
        }

        // Never touch ImGui once teardown has started: the context
        // may already be gone.
        if (s_ready.load() && !s_shuttingDown.load() && s_menuOpen.load()) {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

            ImGuiIO& io = ImGui::GetIO();
            switch (msg) {
                case WM_MOUSEMOVE: case WM_MOUSEWHEEL:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                    // Swallowed unconditionally while the menu is up.
                    // Letting a click reach the game would make it
                    // call setIngameFocus and snatch the cursor back
                    // mid-click.
                    return 0;
                case WM_KEYDOWN: case WM_KEYUP:
                case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
                    if (io.WantCaptureKeyboard) return 0;
                    break;
                default:
                    break;
            }
        }

        return CallWindowProcA(oWndProc, hWnd, msg, wParam, lParam);
    }

    static bool InitImGui(HDC hdc, HGLRC gameCtx) {
        gameWindow = WindowFromDC(hdc);
        if (!gameWindow) return false;

        // wglShareLists has to run before the new context is ever
        // made current, otherwise it fails.
        imguiContext = wglCreateContext(hdc);
        if (!imguiContext) {
            printf("[GLHook] wglCreateContext failed (%lu)\n", GetLastError());
            return false;
        }
        if (!wglShareLists(gameCtx, imguiContext)) {
            printf("[GLHook] wglShareLists failed (%lu), continuing\n", GetLastError());
        }

        if (!wglMakeCurrent(hdc, imguiContext)) {
            printf("[GLHook] wglMakeCurrent failed (%lu)\n", GetLastError());
            wglDeleteContext(imguiContext);
            imguiContext = nullptr;
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;

        if (!ImGui_ImplWin32_Init(gameWindow) || !ImGui_ImplOpenGL2_Init()) {
            printf("[GLHook] ImGui backend init failed\n");
            ImGui::DestroyContext();
            wglDeleteContext(imguiContext);
            imguiContext = nullptr;
            return false;
        }

        // Loads fonts and the style. Must run before the first frame,
        // since the OpenGL2 backend builds the font atlas there.
        Menu::ApplyTheme();

        oWndProc = (WNDPROC)SetWindowLongPtrA(gameWindow, GWLP_WNDPROC,
                                              (LONG_PTR)HookedWndProc);

        // Keybinds only fire while this window has focus
        ModuleManager::SetGameWindow(gameWindow);

        printf("[GLHook] ImGui ready (hwnd %p)\n", (void*)gameWindow);
        return true;
    }

    static BOOL WINAPI HookedSwapBuffers(HDC hdc) {
        // Teardown in progress: pass straight through, touch nothing
        if (s_shuttingDown.load()) return oSwapBuffers(hdc);

        HGLRC gameCtx = wglGetCurrentContext();
        HDC   gameDC  = wglGetCurrentDC();
        if (!gameCtx) return oSwapBuffers(hdc);

        s_inFlight.fetch_add(1);

        // Re-check: Remove() may have raised the flag between the
        // first check and the counter going up.
        if (s_shuttingDown.load()) {
            s_inFlight.fetch_sub(1);
            return oSwapBuffers(hdc);
        }

        if (!s_ready.load()) {
            if (!InitImGui(hdc, gameCtx)) {
                wglMakeCurrent(gameDC, gameCtx);
                s_inFlight.fetch_sub(1);
                return oSwapBuffers(hdc);
            }
            s_ready.store(true);
        }

        if (wglMakeCurrent(hdc, imguiContext)) {
            bool open = s_menuOpen.load();

            // Once the game has let go of the mouse the real system
            // cursor is back on screen, so drawing our own would put
            // two pointers on top of each other. The software cursor
            // is only the fallback for when the grab could not be
            // released.
            ImGui::GetIO().MouseDrawCursor = open && !MouseControl::IsReleased();

            ImGui_ImplOpenGL2_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            Menu::RenderOverlays();   // world overlays, every frame
            Menu::RenderHUD();

            // Called every frame rather than only while open: the
            // menu owns its fade, and skipping the call would cut it
            // off mid-dissolve.
            Menu::Render(open);

            ImGui::Render();
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

            // Give the game back exactly the context it had
            wglMakeCurrent(gameDC, gameCtx);
        }

        s_inFlight.fetch_sub(1);
        return oSwapBuffers(hdc);
    }

public:
    static bool Install() {
        if (MH_Initialize() != MH_OK) {
            printf("[GLHook] MinHook init failed\n");
            return false;
        }

        HMODULE hGL = GetModuleHandleA("opengl32.dll");
        if (!hGL) {
            printf("[GLHook] opengl32.dll not loaded\n");
            return false;
        }

        pSwapBuffers = (void*)GetProcAddress(hGL, "wglSwapBuffers");
        if (!pSwapBuffers) {
            printf("[GLHook] wglSwapBuffers not exported\n");
            return false;
        }

        if (MH_CreateHook(pSwapBuffers, (void*)&HookedSwapBuffers,
                          (void**)&oSwapBuffers) != MH_OK) {
            printf("[GLHook] MH_CreateHook failed\n");
            return false;
        }
        if (MH_EnableHook(pSwapBuffers) != MH_OK) {
            printf("[GLHook] MH_EnableHook failed\n");
            return false;
        }

        printf("[GLHook] hooked wglSwapBuffers at %p\n", pSwapBuffers);
        return true;
    }

    // Safe to call from the client thread while the render thread is
    // still drawing. Returns only once no frame is inside the hook.
    static void Remove() {
        // Closing first means the client thread hands the mouse back
        // before anything is torn down.
        ApplyMenuState(false);

        s_shuttingDown.store(true);

        if (pSwapBuffers) MH_DisableHook(pSwapBuffers);

        // Wait out any frame already past the flag check. A stuck
        // render thread should not hang the eject, so cap the wait.
        for (int i = 0; i < 200 && s_inFlight.load() > 0; i++) Sleep(5);

        if (s_inFlight.load() > 0) {
            printf("[GLHook] WARN: a frame is still in flight, skipping ImGui teardown\n");
            MH_Uninitialize();
            return;   // leaking the context beats crashing the game
        }

        if (gameWindow && oWndProc) {
            SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)oWndProc);
            oWndProc = nullptr;
        }

        if (s_ready.load()) {
            ImGui::GetIO().MouseDrawCursor = false;
            ImGui_ImplOpenGL2_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            s_ready.store(false);
        }

        if (imguiContext) {
            wglDeleteContext(imguiContext);
            imguiContext = nullptr;
        }

        MH_Uninitialize();
    }

    static bool IsMenuOpen() { return s_menuOpen.load(); }
    static void SetMenuOpen(bool v) { ApplyMenuState(v); }
    static HWND GetWindow() { return gameWindow; }
};
