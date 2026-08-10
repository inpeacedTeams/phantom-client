#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdio>

// =================================================================
// KeyCapture
// =================================================================
// A keybind picker needs the NEXT key the user presses, whatever it
// is. That is a window-message problem, not a polling problem:
// GetAsyncKeyState can tell you a key is down, but scanning all 256
// of them every frame to guess which one the user meant picks up
// modifiers, key repeats, and whatever they were already holding.
//
// So the WndProc, which already sees every key event in order,
// hands the first one over and the menu reads it.
//
// THREADS
// The window procedure writes, the render thread reads, the client
// thread never touches it. Two atomics are enough: there is only
// ever one capture in flight, because the UI only lets you open one
// picker at a time.
//
// CANCELLING
//   ESC        abandons the capture, the old bind survives
//   BACKSPACE  clears the bind entirely
//   DELETE     the eject key, so it is refused rather than bound
// =================================================================

class KeyCapture {
public:
    enum Result {
        None = 0,      // still waiting
        Bound,         // a key was captured
        Cleared,       // the user asked to unbind
        Cancelled      // the user backed out
    };

private:
    inline static std::atomic<bool> s_active{ false };
    inline static std::atomic<int>  s_result{ None };
    inline static std::atomic<int>  s_key{ 0 };

    // Modifiers on their own are almost never what someone means: a
    // picker that grabs SHIFT the instant you reach for SHIFT+F is
    // maddening. They are still bindable, just not on their own
    // keydown, so a bare press is ignored and only a real key lands.
    static bool IsBareModifier(int vk) {
        switch (vk) {
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            case VK_MENU: case VK_LMENU: case VK_RMENU:
            case VK_LWIN: case VK_RWIN:
                return true;
            default:
                return false;
        }
    }

public:
    static void Begin() {
        s_key.store(0);
        s_result.store(None);
        s_active.store(true);
    }

    static void Cancel() {
        s_active.store(false);
        s_result.store(Cancelled);
    }

    static bool IsActive() { return s_active.load(); }

    // -------------------------------------------------------------
    // Called from the window procedure. Returns true when the
    // message was consumed and must not reach the game.
    // -------------------------------------------------------------
    static bool OnKeyDown(int vk) {
        if (!s_active.load()) return false;

        if (vk == VK_ESCAPE) {
            s_active.store(false);
            s_result.store(Cancelled);
            return true;
        }

        if (vk == VK_BACK) {
            s_active.store(false);
            s_key.store(0);
            s_result.store(Cleared);
            return true;
        }

        // DELETE ejects the client and INSERT opens this menu.
        // Letting either be bound to a module is a trap.
        if (vk == VK_DELETE || vk == VK_INSERT) return true;

        if (IsBareModifier(vk)) return true;   // swallow, keep waiting

        s_key.store(vk);
        s_active.store(false);
        s_result.store(Bound);
        return true;
    }

    // Mouse buttons make perfectly good binds, but not the two the
    // player fights with.
    static bool OnMouseButton(int vk) {
        if (!s_active.load()) return false;
        if (vk == VK_LBUTTON || vk == VK_RBUTTON) return true;

        s_key.store(vk);
        s_active.store(false);
        s_result.store(Bound);
        return true;
    }

    // -------------------------------------------------------------
    // Read once by the menu. Consumes the result, so the next call
    // returns None until another capture is started.
    // -------------------------------------------------------------
    static Result Poll(int* outKey) {
        int r = s_result.exchange(None);
        if (r == Bound && outKey) *outKey = s_key.load();
        return (Result)r;
    }

    // Human-readable name for a virtual key. Long, but a bind that
    // reads "0xBE" is a bind nobody trusts.
    static void Label(int key, char* out, size_t n) {
        if (n == 0) return;
        out[0] = '\0';
        if (key <= 0) { snprintf(out, n, "None"); return; }

        if (key >= 'A' && key <= 'Z') { snprintf(out, n, "%c", (char)key); return; }
        if (key >= '0' && key <= '9') { snprintf(out, n, "%c", (char)key); return; }
        if (key >= VK_F1 && key <= VK_F24) {
            snprintf(out, n, "F%d", key - VK_F1 + 1);
            return;
        }
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
            snprintf(out, n, "NUM%d", key - VK_NUMPAD0);
            return;
        }

        switch (key) {
            case VK_SPACE:    snprintf(out, n, "SPACE"); return;
            case VK_TAB:      snprintf(out, n, "TAB"); return;
            case VK_RETURN:   snprintf(out, n, "ENTER"); return;
            case VK_SHIFT:
            case VK_LSHIFT:   snprintf(out, n, "SHIFT"); return;
            case VK_RSHIFT:   snprintf(out, n, "RSHIFT"); return;
            case VK_CONTROL:
            case VK_LCONTROL: snprintf(out, n, "CTRL"); return;
            case VK_RCONTROL: snprintf(out, n, "RCTRL"); return;
            case VK_MENU:
            case VK_LMENU:    snprintf(out, n, "ALT"); return;
            case VK_RMENU:    snprintf(out, n, "RALT"); return;
            case VK_CAPITAL:  snprintf(out, n, "CAPS"); return;
            case VK_HOME:     snprintf(out, n, "HOME"); return;
            case VK_END:      snprintf(out, n, "END"); return;
            case VK_PRIOR:    snprintf(out, n, "PGUP"); return;
            case VK_NEXT:     snprintf(out, n, "PGDN"); return;
            case VK_LEFT:     snprintf(out, n, "LEFT"); return;
            case VK_RIGHT:    snprintf(out, n, "RIGHT"); return;
            case VK_UP:       snprintf(out, n, "UP"); return;
            case VK_DOWN:     snprintf(out, n, "DOWN"); return;
            case VK_MBUTTON:  snprintf(out, n, "MOUSE3"); return;
            case VK_XBUTTON1: snprintf(out, n, "MOUSE4"); return;
            case VK_XBUTTON2: snprintf(out, n, "MOUSE5"); return;
            case VK_OEM_3:    snprintf(out, n, "`"); return;
            case VK_OEM_MINUS:snprintf(out, n, "-"); return;
            case VK_OEM_PLUS: snprintf(out, n, "="); return;
            case VK_OEM_4:    snprintf(out, n, "["); return;
            case VK_OEM_6:    snprintf(out, n, "]"); return;
            case VK_OEM_1:    snprintf(out, n, ";"); return;
            case VK_OEM_7:    snprintf(out, n, "'"); return;
            case VK_OEM_COMMA:snprintf(out, n, ","); return;
            case VK_OEM_PERIOD: snprintf(out, n, "."); return;
            case VK_OEM_2:    snprintf(out, n, "/"); return;
            case VK_OEM_5:    snprintf(out, n, "\\"); return;
            default: break;
        }

        // Last resort: ask Windows what the key is called on this
        // layout, so non-US keyboards still read sensibly.
        UINT sc = MapVirtualKeyA((UINT)key, MAPVK_VK_TO_VSC);
        if (sc != 0) {
            char buf[64] = {};
            if (GetKeyNameTextA((LONG)(sc << 16), buf, sizeof(buf)) > 0) {
                snprintf(out, n, "%s", buf);
                return;
            }
        }
        snprintf(out, n, "KEY%d", key);
    }
};
