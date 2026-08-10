#pragma once
#include <Windows.h>
#include <atomic>

// =================================================================
// Focus
// =================================================================
// Is the game window the foreground window right now?
//
// WHY THIS IS SHARED AND NOT PER-MODULE
//
// Several modules poll GetAsyncKeyState(VK_LBUTTON) to decide
// whether the player is holding attack. That call is GLOBAL: it
// returns the physical button state no matter which window has
// focus. So while the game is alt-tabbed, a click in another
// application still reads as the player holding the button, and a
// module keyed on it would fire on input that was never aimed at
// the game.
//
// The click engine already screened for this with its own
// foreground check, but it held a private HWND and the modules had
// no way to ask the same question, so each ended up with a
// different guard or none. Rather than copy the check a fourth
// time, the window handle lives here once and everyone reads it.
//
// Modules cannot reach ModuleManager (it includes them, so the
// dependency only runs one way), which is exactly why this is a
// tiny leaf header with no other dependencies: every module can
// include it, and ModuleManager publishes the handle into it from
// SetGameWindow.
//
// THREADS
// The handle is set once, early, from the render/window thread and
// read from the client tick and the click timer. A single atomic
// covers all of that; there is no ordering to get wrong.
// =================================================================

class Focus {
private:
    inline static std::atomic<HWND> s_window{ nullptr };

public:
    // Published by ModuleManager::SetGameWindow once the overlay has
    // found the game's HWND.
    static void SetWindow(HWND hwnd) {
        s_window.store(hwnd, std::memory_order_relaxed);
    }

    static HWND Window() { return s_window.load(std::memory_order_relaxed); }

    // True when the game window is foreground, OR when the handle is
    // not known yet. The fallback is deliberate: before the overlay
    // has resolved the window, refusing every input would make the
    // affected modules look broken, and there is no menu to steal
    // focus at that point anyway.
    static bool GameFocused() {
        HWND w = s_window.load(std::memory_order_relaxed);
        return w == nullptr || GetForegroundWindow() == w;
    }

    // Physical mouse/key state, but only when the game actually has
    // focus. This is what a module means when it asks "is the player
    // holding this", as opposed to "is this key down somewhere in
    // Windows". Modules should prefer this over a bare
    // GetAsyncKeyState.
    static bool KeyHeld(int vk) {
        return GameFocused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
};
