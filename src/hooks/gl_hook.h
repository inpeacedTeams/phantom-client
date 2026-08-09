#pragma once
#include <Windows.h>
#include <gl/GL.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>
#include <cstdio>

#include "../gui/menu.h"

// Forward decl for ImGui WndProc handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef BOOL(WINAPI* fnWglSwapBuffers)(HDC);
typedef LRESULT(WINAPI* fnWndProc)(HWND, UINT, WPARAM, LPARAM);

class GLHook {
private:
    inline static fnWglSwapBuffers oSwapBuffers = nullptr;
    inline static fnWndProc oWndProc = nullptr;
    inline static HWND gameWindow = nullptr;
    inline static HGLRC imguiContext = nullptr;
    inline static HGLRC gameContext = nullptr;
    inline static bool initialized = false;
    inline static bool menuOpen = false;

    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        // Toggle menu with INSERT
        if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
            menuOpen = !menuOpen;
            return 0;
        }

        // Forward to ImGui when menu is open
        if (menuOpen) {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

            // Block game input when menu is open
            if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
                msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MOUSEWHEEL ||
                msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR) {
                if (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard) {
                    return 0;
                }
            }
        }

        return CallWindowProcA((WNDPROC)oWndProc, hWnd, msg, wParam, lParam);
    }

    static BOOL WINAPI HookedSwapBuffers(HDC hdc) {
        if (!initialized) {
            // Save game's GL context
            gameContext = wglGetCurrentContext();
            gameWindow = WindowFromDC(hdc);

            // Create our own GL context for ImGui
            imguiContext = wglCreateContext(hdc);
            wglMakeCurrent(hdc, imguiContext);

            // Share textures/display lists with game context
            wglShareLists(gameContext, imguiContext);

            // Init ImGui
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr; // Don't save imgui.ini

            ImGui_ImplWin32_Init(gameWindow);
            ImGui_ImplOpenGL2_Init();

            // Hook WndProc for input
            oWndProc = (fnWndProc)SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

            // Apply theme
            Menu::ApplyTheme();

            initialized = true;
            printf("[GLHook] ImGui initialized\n");
        }

        // Switch to our GL context
        wglMakeCurrent(hdc, imguiContext);

        // Start ImGui frame
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render our menu
        if (menuOpen) {
            Menu::Render();
        }

        // Always render the active modules HUD
        Menu::RenderHUD();

        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

        // Switch back to game's GL context
        wglMakeCurrent(hdc, gameContext);

        return oSwapBuffers(hdc);
    }

public:
    static bool Install() {
        if (MH_Initialize() != MH_OK) {
            printf("[GLHook] MinHook init failed\n");
            return false;
        }

        HMODULE hOpenGL = GetModuleHandleA("opengl32.dll");
        if (!hOpenGL) {
            printf("[GLHook] opengl32.dll not found\n");
            return false;
        }

        void* pSwapBuffers = GetProcAddress(hOpenGL, "wglSwapBuffers");
        if (!pSwapBuffers) {
            printf("[GLHook] wglSwapBuffers not found\n");
            return false;
        }

        if (MH_CreateHook(pSwapBuffers, &HookedSwapBuffers, (void**)&oSwapBuffers) != MH_OK) {
            printf("[GLHook] Hook creation failed\n");
            return false;
        }

        if (MH_EnableHook(pSwapBuffers) != MH_OK) {
            printf("[GLHook] Hook enable failed\n");
            return false;
        }

        printf("[GLHook] wglSwapBuffers hooked at %p\n", pSwapBuffers);
        return true;
    }

    static void Remove() {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

        if (gameWindow && oWndProc) {
            SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)oWndProc);
        }

        if (initialized) {
            ImGui_ImplOpenGL2_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
    }

    static bool IsMenuOpen() { return menuOpen; }
    static void SetMenuOpen(bool open) { menuOpen = open; }
};
