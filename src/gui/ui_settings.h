#pragma once
#include <imgui.h>
#include <Windows.h>
#include <mmsystem.h>
#include <cstring>

#pragma comment(lib, "winmm.lib")

// =================================================================
// UI settings
// =================================================================
// Everything the Interface tab can change lives here as plain data.
// The theme reads it and rewrites the palette and the metrics in
// place, so no widget has to know these settings exist.
//
// That indirection is the whole point. The alternative is every
// draw call asking "what is the accent, what is the scale", which
// is how a UI ends up half-themed: the two places somebody forgot
// stay blue forever.
//
// -----------------------------------------------------------------
// ON BLUR
// -----------------------------------------------------------------
// A real frosted-glass blur means capturing the back buffer into a
// texture and running a separable gaussian over it every frame.
// Inside an injected overlay that shares a GL context with the game
// that is a genuinely risky thing to do: it touches texture state,
// the framebuffer binding and the matrix stack in the middle of
// somebody else's render loop, and getting any of it wrong takes
// the game down rather than looking slightly wrong.
//
// The client has crashed enough for one week. What is here instead
// is a graded dim with a vignette, which reads as depth, costs two
// rectangles, and cannot break anything. If the real thing is worth
// it later it belongs behind its own hook, not bolted onto this.
//
// -----------------------------------------------------------------
// ON HOVER SOUNDS
// -----------------------------------------------------------------
// Dropped on purpose. A sound that fires every time the pointer
// crosses a row fires forty times while you scan a list, and there
// is no volume at which that is pleasant. Click feedback is kept,
// off by default, because Windows only offers system aliases and
// they sound like an error dialog.
// =================================================================

namespace iOS {

struct AccentPreset {
    const char* name;
    int r, g, b;
};

inline const AccentPreset kAccents[] = {
    { "Blue",     0, 122, 255 },
    { "Purple", 175,  82, 222 },
    { "Pink",   255,  45,  85 },
    { "Teal",    48, 176, 199 },
    { "Green",   52, 199,  89 },
    { "Orange", 255, 149,   0 },
    { "Red",    255,  59,  48 },
    { "Slate",  108, 120, 141 },
};
inline constexpr int kAccentCount = (int)(sizeof(kAccents) / sizeof(kAccents[0]));

struct UISettings {
    // ---- Layout ----
    float scale     = 1.0f;    // 0.8 to 1.4
    float rounding  = 1.0f;    // multiplier on every corner radius

    // ---- Motion ----
    float animSpeed = 1.0f;    // multiplier on every easing rate
    bool  openAnim  = true;    // the lift and stagger on open

    // ---- Colour ----
    int   accent    = 0;       // index into kAccents
    float glow      = 1.0f;    // halo and glow intensity

    // ---- Backdrop ----
    float dim       = 0.42f;   // how far the world drops back
    bool  vignette  = true;

    // ---- Feedback ----
    bool  soundOnToggle = false;
    bool  soundOnOpen   = false;
};

class UI {
private:
    inline static UISettings s_s;
    inline static bool s_dirty = true;

public:
    static UISettings& Mut()      { s_dirty = true; return s_s; }
    static const UISettings& Get() { return s_s; }

    // The theme polls this once a frame and rebuilds the palette
    // only when something actually moved.
    static bool TakeDirty() {
        if (!s_dirty) return false;
        s_dirty = false;
        return true;
    }

    static void MarkDirty() { s_dirty = true; }

    static float Scale()     { return s_s.scale; }
    static float AnimSpeed() { return s_s.animSpeed; }
    static float Glow()      { return s_s.glow; }

    // Scale a hand-written pixel value. Used by the few widgets that
    // cannot go through the shared metrics.
    static float S(float px) { return px * s_s.scale; }

    static ImU32 Accent(float alpha = 1.0f) {
        const AccentPreset& a = kAccents[
            (s_s.accent < 0 || s_s.accent >= kAccentCount) ? 0 : s_s.accent];
        return IM_COL32(a.r, a.g, a.b, (int)(255 * alpha));
    }

    static const char* AccentName() {
        const AccentPreset& a = kAccents[
            (s_s.accent < 0 || s_s.accent >= kAccentCount) ? 0 : s_s.accent];
        return a.name;
    }

    // ---- Feedback ----
    // Windows system aliases, because shipping a wav inside a DLL
    // for a click is not worth the bytes. Async so the render
    // thread never waits on the audio device.
    static void PlayToggle() {
        if (!s_s.soundOnToggle) return;
        PlaySoundA("SystemAsterisk", nullptr,
                   SND_ALIAS | SND_ASYNC | SND_NODEFAULT | SND_NOWAIT);
    }

    static void PlayOpen() {
        if (!s_s.soundOnOpen) return;
        PlaySoundA("SystemDefault", nullptr,
                   SND_ALIAS | SND_ASYNC | SND_NODEFAULT | SND_NOWAIT);
    }

    static void Reset() {
        s_s = UISettings();
        s_dirty = true;
    }
};

} // namespace iOS
