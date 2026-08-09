#pragma once
#include <Windows.h>
#include <gl/GL.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>
#include <cstdio>

#include "../gui/menu.h"
#include "../modules/module_manager.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef BOOL(WINAPI* fnWglSwapBuffers)(HDC);

class GLHook {
private:
    inline static fnWglSwapBuffers oSwapBuffers = nullptr;
    inline static WNDPROC  oWndProc     = nullptr;
    inline static HWND     gameWindow   = nullptr;
    inline static HGLRC    imguiContext = nullptr;
    inline static bool     initialized  = false;
    inline static bool     menuOpen     = false;
    inline static void*    pSwapBuffers = nullptr;

    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
            menuOpen = !menuOpen;
            return 0;
        }

        if (initialized && menuOpen) {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

            ImGuiIO& io = ImGui::GetIO();
            switch (msg) {
                case WM_MOUSEMOVE: case WM_MOUSEWHEEL:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                    if (io.WantCaptureMouse) return 0;
                    break;
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

        // Create our own context and share the game's objects with it.
        // wglShareLists must run BEFORE the new context is ever made
        // current, otherwise it fails.
        imguiContext = wglCreateContext(hdc);
        if (!imguiContext) {
            printf("[GLHook] wglCreateContext failed (%lu)\n", GetLastError());
            return false;
        }
        if (!wglShareLists(gameCtx, imguiContext)) {
            printf("[GLHook] wglShareLists failed (%lu), continuing anyway\n", GetLastError());
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
            return false;
        }

        Menu::ApplyTheme();

        oWndProc = (WNDPROC)SetWindowLongPtrA(gameWindow, GWLP_WNDPROC,
                                              (LONG_PTR)HookedWndProc);

        // Keybinds should only fire while this window is focused
        ModuleManager::SetGameWindow(gameWindow);

        printf("[GLHook] ImGui ready (hwnd %p)\n", (void*)gameWindow);
        return true;
    }

    static BOOL WINAPI HookedSwapBuffers(HDC hdc) {
        HGLRC gameCtx = wglGetCurrentContext();
        HDC   gameDC  = wglGetCurrentDC();

        if (!gameCtx) return oSwapBuffers(hdc);

        if (!initialized) {
            if (!InitImGui(hdc, gameCtx)) {
                wglMakeCurrent(gameDC, gameCtx);
                return oSwapBuffers(hdc);
            }
            initialized = true;
        }

        if (!wglMakeCurrent(hdc, imguiContext)) {
            return oSwapBuffers(hdc);
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // World overlays draw every frame; the menu only when open.
        Menu::RenderOverlays();
        Menu::RenderHUD();
        if (menuOpen) Menu::Render();

        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

        // Hand the game back exactly the context it had
        wglMakeCurrent(gameDC, gameCtx);

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
            printf("[GLHook] wglSwapBuffers not found\n");
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

    static void Remove() {
        if (pSwapBuffers) MH_DisableHook(pSwapBuffers);

        // Let any in-flight frame finish before tearing ImGui down
        Sleep(120);

        if (gameWindow && oWndProc) {
            SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)oWndProc);
            oWndProc = nullptr;
        }

        if (initialized) {
            ImGui_ImplOpenGL2_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            initialized = false;
        }

        if (imguiContext) {
            wglDeleteContext(imguiContext);
            imguiContext = nullptr;
        }

        MH_Uninitialize();
    }

    static bool IsMenuOpen() { return menuOpen; }
    static void SetMenuOpen(bool v) { menuOpen = v; }
    static HWND GetWindow() { return gameWindow; }
};
