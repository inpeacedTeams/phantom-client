#pragma once
#include <imgui.h>
#include <unordered_map>
#include <cmath>

// =================================================================
// iOS design system
// =================================================================
// Colours, metrics and the animation store the widgets run on.
//
// The palette is the real iOS light system palette. Grouped grey
// behind, white cards on top, one accent for anything interactive,
// green only for a switch that is on. Nothing else is coloured,
// which is what makes an iOS screen feel calm.
//
// -----------------------------------------------------------------
// WHY THESE ARE VARIABLES AND NOT CONSTANTS
// -----------------------------------------------------------------
// The Interface tab lets the user change the accent, the scale and
// the corner radius at runtime. Every widget in the client already
// reads Col::Blue and M::RowHeight, so the cheapest correct way to
// make those settings real is to let UI::Apply() write into them
// once, rather than thread a theme object through forty call sites.
//
// They are only ever written from the render thread, inside Apply,
// and only ever read from the render thread while drawing. There is
// no second writer, so no lock is needed.
// =================================================================

namespace iOS {

// ---- Palette (iOS light) ----
namespace Col {
    // Surfaces
    inline ImU32 GroupedBg   = IM_COL32(242, 242, 247, 255);  // behind cards
    inline ImU32 Card        = IM_COL32(255, 255, 255, 255);
    inline ImU32 CardPressed = IM_COL32(229, 229, 234, 255);
    inline ImU32 Fill        = IM_COL32(120, 120, 128, 51);   // track, chips
    inline ImU32 FillThick   = IM_COL32(120, 120, 128, 92);
    inline ImU32 Separator   = IM_COL32(198, 198, 200, 140);

    // Text
    inline ImU32 Label       = IM_COL32(0, 0, 0, 255);
    inline ImU32 Label2      = IM_COL32(60, 60, 67, 153);     // 60%
    inline ImU32 Label3      = IM_COL32(60, 60, 67, 76);      // 30%
    inline ImU32 OnAccent    = IM_COL32(255, 255, 255, 255);

    // The accent. Written by UI::Apply, read everywhere. Named Blue
    // because that is what it starts as and because renaming it
    // would touch every file for no gain.
    inline ImU32 Blue        = IM_COL32(0, 122, 255, 255);
    inline ImU32 BlueSoft    = IM_COL32(0, 122, 255, 38);

    // Fixed hues, never themed
    inline const ImU32 Green  = IM_COL32(52, 199, 89, 255);
    inline const ImU32 Red    = IM_COL32(255, 59, 48, 255);
    inline const ImU32 Orange = IM_COL32(255, 149, 0, 255);
    inline const ImU32 Yellow = IM_COL32(255, 204, 0, 255);
    inline const ImU32 Purple = IM_COL32(175, 82, 222, 255);
    inline const ImU32 Teal   = IM_COL32(48, 176, 199, 255);
    inline const ImU32 Pink   = IM_COL32(255, 45, 85, 255);

    // Shadow, layered rather than blurred
    inline const ImU32 Shadow1 = IM_COL32(0, 0, 0, 10);
    inline const ImU32 Shadow2 = IM_COL32(0, 0, 0, 6);

    inline ImU32 Alpha(ImU32 c, float a) {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
        v.w *= a;
        return ImGui::ColorConvertFloat4ToU32(v);
    }

    inline ImU32 Mix(ImU32 a, ImU32 b, float t) {
        ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
        ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(
            x.x + (y.x - x.x) * t,
            x.y + (y.y - x.y) * t,
            x.z + (y.z - x.z) * t,
            x.w + (y.w - x.w) * t));
    }
}

// =================================================================
// Metrics
// =================================================================
// Base values are the real iOS numbers. The live values are those
// multiplied by the user's scale, so a 1.2x menu is genuinely
// bigger rather than just having bigger text in the same boxes.
// =================================================================
namespace M {
    inline constexpr float BaseCardRadius = 12.0f;
    inline constexpr float BaseRowHeight  = 44.0f;   // the iOS row height
    inline constexpr float BaseRowPadX    = 16.0f;
    inline constexpr float BaseSwitchW    = 51.0f;   // exact iOS switch
    inline constexpr float BaseSwitchH    = 31.0f;
    inline constexpr float BaseSwitchKnob = 27.0f;
    inline constexpr float BaseSegHeight  = 32.0f;
    inline constexpr float BaseSegRadius  = 8.0f;
    inline constexpr float BaseSheetRound = 20.0f;

    inline float CardRadius = BaseCardRadius;
    inline float RowHeight  = BaseRowHeight;
    inline float RowPadX    = BaseRowPadX;
    inline float SwitchW    = BaseSwitchW;
    inline float SwitchH    = BaseSwitchH;
    inline float SwitchKnob = BaseSwitchKnob;
    inline float SegHeight  = BaseSegHeight;
    inline float SegRadius  = BaseSegRadius;
    inline float SheetRound = BaseSheetRound;
    inline float CardGap    = 22.0f;
}

// =================================================================
// Easing
// =================================================================
// One-shot curves for progress values that already run 0..1 over
// time: an intro playing, a notification leaving, a tab arriving.
//
// These are STATELESS on purpose. Anything that has to REMEMBER a
// value between frames uses Anim below; anything that just needs to
// shape a number it already holds uses these. They live here, once,
// because the same ease-out cubic hand-copied into three files is
// how two of the three quietly end up different.
// =================================================================
namespace Ease {
    inline float Clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // Decelerating cubic. Fast off the mark, settles softly, which
    // is the curve iOS uses for anything entering the screen.
    inline float Out(float t) {
        float u = 1.0f - Clamp01(t);
        return 1.0f - u * u * u;
    }

    // Accelerating quadratic. Used on the way out so a thing leaves
    // with intent instead of dribbling away.
    inline float In(float t) {
        float u = Clamp01(t);
        return u * u;
    }

    // Symmetric, for something that both starts and stops on screen.
    inline float InOut(float t) {
        t = Clamp01(t);
        return t < 0.5f
            ? 2.0f * t * t
            : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * 0.5f;
    }

    // Overshoots a hair past the target and settles back, for a
    // control that should feel like it carries a little weight.
    inline float OutBack(float t) {
        t = Clamp01(t);
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
}

// =================================================================
// Interface settings
// =================================================================
// Everything the user can change about the look of the client.
//
// WHAT IS DELIBERATELY NOT HERE
//
//   Real background blur. A gaussian blur needs the framebuffer
//   read back into a texture and a shader pass. This overlay runs
//   on imgui_impl_opengl2, the fixed-function backend, chosen
//   because it cannot disturb the game's GL state. Faking blur by
//   stacking translucent quads costs fill rate and looks like a
//   smear. A clean dim plus a vignette reads better and is free, so
//   the setting is honestly called Dim.
//
//   Interface sounds. There is no asset pipeline in a single
//   injected DLL, and the alternative is PlaySound with a Windows
//   system beep, which sounds like an error dialog and plays
//   through the wrong device. A UI that clicks at you in a game
//   with its own audio is worse than a silent one.
// =================================================================
struct UI {
    // Layout
    inline static float scale     = 1.00f;   // 0.85 - 1.35
    inline static float animSpeed = 1.00f;   // 0.4 lazy, 2.0 snappy
    inline static float roundness = 1.00f;   // multiplier on every radius

    // Effects
    inline static bool  glow        = true;
    inline static float glowAmount  = 1.00f;
    inline static bool  dim         = true;
    inline static float dimAmount   = 0.42f;
    inline static bool  vignette    = true;

    // Behaviour
    inline static bool  openAnimation = true;
    inline static bool  hoverInfo     = true;   // description card on hover
    inline static bool  rowNudge      = true;   // hovered row slides right

    // Accent
    inline static int   accent = 0;
    inline static float accentCustom[4] = { 0.00f, 0.48f, 1.00f, 1.00f };

    // -------------------------------------------------------------
    // Animated live values
    // -------------------------------------------------------------
    // The fields above are TARGETS the user sets. These are what the
    // client actually draws with, eased toward the targets once a
    // frame so changing the accent, the scale or the corner radius
    // glides rather than snapping. A slider used to resize the whole
    // menu one jump per pixel of travel; now it sets a target and
    // the size slides in.
    //
    // Written only from the render thread, inside Apply. Read only
    // from the render thread while drawing. No second writer.
    // -------------------------------------------------------------
    inline static float liveScale     = 1.00f;
    inline static float liveRoundness = 1.00f;
    inline static float liveAccent[4] = { 0.00f, 0.48f, 1.00f, 1.00f };
    inline static bool  liveReady     = false;   // has a render frame primed it

    static constexpr int kAccentCount = 8;

    static const char* const* AccentNames() {
        static const char* names[kAccentCount] = {
            "Blue", "Purple", "Pink", "Teal",
            "Green", "Orange", "Red", "Custom"
        };
        return names;
    }

    static ImU32 AccentColor() {
        switch (accent) {
            case 1: return Col::Purple;
            case 2: return Col::Pink;
            case 3: return Col::Teal;
            case 4: return Col::Green;
            case 5: return Col::Orange;
            case 6: return Col::Red;
            case 7: return ImGui::ColorConvertFloat4ToU32(ImVec4(
                        accentCustom[0], accentCustom[1],
                        accentCustom[2], accentCustom[3]));
            default: return IM_COL32(0, 122, 255, 255);
        }
    }

    // The target accent as straight floats, so the live value can be
    // eased in colour space rather than by re-packing ImU32 every
    // frame. Pure maths, safe with no ImGui context.
    static void AccentColorF(float out[4]) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(AccentColor());
        out[0] = c.x; out[1] = c.y; out[2] = c.z; out[3] = c.w;
    }

    static float Clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // -------------------------------------------------------------
    // Bring every value back into range.
    //
    // Separate from Apply because the config is loaded on the CLIENT
    // thread at inject time, which is BEFORE the overlay exists.
    // Anything that touches ImGui state would be dereferencing a
    // null context, and "the client crashes the game on startup if
    // you hand-edit a config" is not a thing a finished product
    // does.
    // -------------------------------------------------------------
    static void ClampValues() {
        scale      = Clamp(scale, 0.85f, 1.35f);
        animSpeed  = Clamp(animSpeed, 0.40f, 2.00f);
        roundness  = Clamp(roundness, 0.00f, 1.60f);
        glowAmount = Clamp(glowAmount, 0.20f, 2.00f);
        dimAmount  = Clamp(dimAmount, 0.00f, 0.80f);

        if (accent < 0 || accent >= kAccentCount) accent = 0;
        for (int i = 0; i < 4; i++)
            accentCustom[i] = Clamp(accentCustom[i], 0.0f, 1.0f);

        // A fully transparent custom accent would make every
        // interactive element invisible.
        if (accentCustom[3] < 0.35f) accentCustom[3] = 1.0f;
    }

    // Push the settings into the palette and the metrics. Called
    // once per frame before anything draws.
    //
    // The user's values are TARGETS; the live values chase them with
    // the same frame-rate-independent smoothing every widget uses,
    // so the accent, the scale and the roundness all glide. On the
    // very first render frame, and any time this runs with no ImGui
    // context (a config load on the client thread), there is nothing
    // to animate from and no clock to animate against, so it snaps.
    static void Apply() {
        ClampValues();

        float tgtAccent[4];
        AccentColorF(tgtAccent);

        bool  haveCtx = ImGui::GetCurrentContext() != nullptr;
        float dt = haveCtx ? ImGui::GetIO().DeltaTime : 0.0f;
        if (dt > 0.1f) dt = 0.1f;    // survive a stall without a jump

        if (!liveReady) {
            // First sight: snap, because a glide from a default that
            // was never on screen is just a flash of the wrong theme.
            liveScale     = scale;
            liveRoundness = roundness;
            for (int i = 0; i < 4; i++) liveAccent[i] = tgtAccent[i];
            if (haveCtx) liveReady = true;   // only once there is a clock
        } else if (haveCtx) {
            float k = 1.0f - std::exp(-14.0f * animSpeed * dt);
            liveScale     += (scale     - liveScale)     * k;
            liveRoundness += (roundness - liveRoundness) * k;
            for (int i = 0; i < 4; i++)
                liveAccent[i] += (tgtAccent[i] - liveAccent[i]) * k;
        }
        // else: applied off the render thread after priming. Leave
        // the live values alone so the next render frame glides from
        // where the eye last saw them to the new target.

        Col::Blue = ImGui::ColorConvertFloat4ToU32(ImVec4(
            liveAccent[0], liveAccent[1], liveAccent[2], liveAccent[3]));
        Col::BlueSoft = Col::Alpha(Col::Blue, 0.15f);

        const float s = liveScale;
        const float r = liveScale * liveRoundness;

        M::RowHeight  = M::BaseRowHeight  * s;
        M::RowPadX    = M::BaseRowPadX    * s;
        M::SwitchW    = M::BaseSwitchW    * s;
        M::SwitchH    = M::BaseSwitchH    * s;
        M::SwitchKnob = M::BaseSwitchKnob * s;
        M::SegHeight  = M::BaseSegHeight  * s;

        M::CardRadius = M::BaseCardRadius * r;
        M::SegRadius  = M::BaseSegRadius  * r;
        M::SheetRound = M::BaseSheetRound * r;

        if (!haveCtx) return;

        // Text scales with everything else, or a 1.3x menu is just
        // the same small type in roomier boxes.
        ImGui::GetIO().FontGlobalScale = s;

        ImGuiStyle& st = ImGui::GetStyle();
        st.FrameRounding  = 9.0f * r;
        st.ChildRounding  = M::CardRadius;
        st.GrabRounding   = 10.0f * r;
        st.PopupRounding  = 12.0f * r;
        st.WindowRounding = M::SheetRound;
    }

    static void Reset() {
        scale = animSpeed = roundness = 1.0f;
        glow = dim = vignette = true;
        glowAmount = 1.0f;
        dimAmount = 0.42f;
        openAnimation = hoverInfo = rowNudge = true;
        accent = 0;
        accentCustom[0] = 0.00f; accentCustom[1] = 0.48f;
        accentCustom[2] = 1.00f; accentCustom[3] = 1.00f;
        // liveReady is left as-is on purpose: the values above are
        // targets, and the live ones glide to them, so Reset reads
        // as the interface easing back to default rather than a cut.
    }
};

// =================================================================
// Animation store
// =================================================================
// Widgets are redrawn from scratch every frame, so any animated
// value has to live somewhere keyed by widget identity.
//
// The step is exponential smoothing against real elapsed time:
//   v += (target - v) * (1 - exp(-speed * dt))
// Frame-rate independent, so it looks identical at 60 and at 400
// FPS. A plain `v += (target - v) * 0.2f` would not: the higher the
// frame rate, the faster the animation, which is why so many menus
// feel different depending on the machine.
// =================================================================

class Anim {
private:
    struct Entry {
        float value = 0.0f;
        int   frame = 0;
    };
    inline static std::unordered_map<ImGuiID, Entry> s_store;
    inline static int s_gcCounter = 0;

public:
    // Ease toward target. speed 10 is brisk, 18 snappy, 6 lazy.
    // Scaled by the user's animation speed, which is the only place
    // that setting needs to exist.
    static float To(ImGuiID id, float target, float speed = 14.0f) {
        ImGuiIO& io = ImGui::GetIO();
        Entry& e = s_store[id];

        if (e.frame == 0) e.value = target;   // no animation on first sight
        e.frame = ImGui::GetFrameCount();

        float dt = io.DeltaTime;
        if (dt > 0.1f) dt = 0.1f;             // survive a stall without a jump

        float sp = speed * UI::animSpeed;
        if (sp < 0.5f) sp = 0.5f;

        e.value += (target - e.value) * (1.0f - std::exp(-sp * dt));

        if (std::fabs(target - e.value) < 0.0005f) e.value = target;
        return e.value;
    }

    static float ToStr(const char* key, float target, float speed = 14.0f) {
        return To(ImGui::GetID(key), target, speed);
    }

    static float Value(ImGuiID id) {
        auto it = s_store.find(id);
        return it == s_store.end() ? 0.0f : it->second.value;
    }

    static void Set(ImGuiID id, float v) {
        Entry& e = s_store[id];
        e.value = v;
        e.frame = ImGui::GetFrameCount();
    }

    // Drop entries for widgets nobody has drawn in a while, so the
    // map does not grow for the life of the process.
    static void GarbageCollect() {
        if (++s_gcCounter < 600) return;
        s_gcCounter = 0;

        int now = ImGui::GetFrameCount();
        for (auto it = s_store.begin(); it != s_store.end(); ) {
            if (now - it->second.frame > 600) it = s_store.erase(it);
            else ++it;
        }
    }

    static void Clear() { s_store.clear(); }
};

// =================================================================
// Fonts
// =================================================================
// The stock ImGui font is a 13px bitmap and no amount of styling
// hides it. Segoe UI is on every Windows install and is close
// enough to SF Pro in proportion to read as the same design.
// =================================================================

struct Fonts {
    inline static ImFont* Body     = nullptr;   // 17px, iOS body
    inline static ImFont* BodyBold = nullptr;
    inline static ImFont* Title    = nullptr;   // 24px
    inline static ImFont* Caption  = nullptr;   // 13px
    inline static ImFont* Mono     = nullptr;

    static void Load() {
        ImGuiIO& io = ImGui::GetIO();

        ImFontConfig cfg;
        cfg.OversampleH = 3;
        cfg.OversampleV = 2;
        cfg.PixelSnapH  = false;

        const char* ui     = "C:\\Windows\\Fonts\\segoeui.ttf";
        const char* uiSemi = "C:\\Windows\\Fonts\\seguisb.ttf";
        const char* uiBold = "C:\\Windows\\Fonts\\segoeuib.ttf";
        const char* mono   = "C:\\Windows\\Fonts\\consola.ttf";

        // Cyrillic too: module names are English but a config name
        // or anything the user types may not be.
        const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();

        Body = io.Fonts->AddFontFromFileTTF(ui, 17.0f, &cfg, ranges);
        if (!Body) {
            // No Segoe UI: fall back and leave every pointer null so
            // the widgets simply skip PushFont.
            io.Fonts->AddFontDefault();
            Body = nullptr;
            return;
        }

        BodyBold = io.Fonts->AddFontFromFileTTF(uiSemi, 17.0f, &cfg, ranges);
        if (!BodyBold) BodyBold = io.Fonts->AddFontFromFileTTF(uiBold, 17.0f, &cfg, ranges);

        Title   = io.Fonts->AddFontFromFileTTF(uiBold, 24.0f, &cfg, ranges);
        Caption = io.Fonts->AddFontFromFileTTF(ui, 13.0f, &cfg, ranges);
        Mono    = io.Fonts->AddFontFromFileTTF(mono, 14.0f, &cfg, ranges);

        io.FontDefault = Body;
    }

    static void Push(ImFont* f) { if (f) ImGui::PushFont(f); }
    static void Pop(ImFont* f)  { if (f) ImGui::PopFont(); }
};

// =================================================================
// Base ImGui style
// =================================================================
// Module settings panels still use stock ImGui widgets. Rather than
// rewrite fifteen modules, the base style is pulled toward the same
// language: rounded, roomy, accented, no borders.
// =================================================================

inline void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = M::SheetRound;
    s.ChildRounding     = M::CardRadius;
    s.FrameRounding     = 9.0f;
    s.GrabRounding      = 10.0f;
    s.PopupRounding     = 12.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding       = 8.0f;

    s.WindowPadding     = ImVec2(0, 0);      // the shell draws its own
    s.FramePadding      = ImVec2(12, 7);
    s.ItemSpacing       = ImVec2(10, 9);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.CellPadding       = ImVec2(8, 6);
    s.ScrollbarSize     = 9.0f;
    s.GrabMinSize       = 22.0f;

    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 0.0f;

    s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;
    s.CurveTessellationTol = 0.9f;

    ImVec4* c = s.Colors;
    auto F = [](ImU32 v) { return ImGui::ColorConvertU32ToFloat4(v); };

    c[ImGuiCol_Text]                 = F(Col::Label);
    c[ImGuiCol_TextDisabled]         = F(Col::Label2);
    c[ImGuiCol_WindowBg]             = F(Col::GroupedBg);
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = F(Col::Card);
    c[ImGuiCol_Border]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]              = F(Col::Fill);
    c[ImGuiCol_FrameBgHovered]       = F(Col::FillThick);
    c[ImGuiCol_FrameBgActive]        = F(Col::FillThick);

    c[ImGuiCol_TitleBg]              = F(Col::Card);
    c[ImGuiCol_TitleBgActive]        = F(Col::Card);
    c[ImGuiCol_TitleBgCollapsed]     = F(Col::Card);
    c[ImGuiCol_MenuBarBg]            = F(Col::Card);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = F(Col::Label3);
    c[ImGuiCol_ScrollbarGrabHovered] = F(Col::Label2);
    c[ImGuiCol_ScrollbarGrabActive]  = F(Col::Label2);

    c[ImGuiCol_CheckMark]            = F(Col::OnAccent);
    c[ImGuiCol_SliderGrab]           = F(Col::Card);
    c[ImGuiCol_SliderGrabActive]     = F(Col::Card);

    c[ImGuiCol_Button]               = F(Col::BlueSoft);
    c[ImGuiCol_ButtonHovered]        = F(Col::Alpha(Col::Blue, 0.24f));
    c[ImGuiCol_ButtonActive]         = F(Col::Alpha(Col::Blue, 0.34f));

    c[ImGuiCol_Header]               = F(Col::Alpha(Col::Blue, 0.10f));
    c[ImGuiCol_HeaderHovered]        = F(Col::Alpha(Col::Blue, 0.16f));
    c[ImGuiCol_HeaderActive]         = F(Col::Alpha(Col::Blue, 0.22f));

    c[ImGuiCol_Separator]            = F(Col::Separator);
    c[ImGuiCol_SeparatorHovered]     = F(Col::Separator);
    c[ImGuiCol_SeparatorActive]      = F(Col::Blue);

    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = F(Col::Label3);
    c[ImGuiCol_ResizeGripActive]     = F(Col::Label2);

    c[ImGuiCol_Tab]                  = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered]           = F(Col::Card);
    c[ImGuiCol_TabActive]            = F(Col::Card);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabUnfocusedActive]   = F(Col::Card);

    c[ImGuiCol_NavHighlight]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, 0.28f);
}

} // namespace iOS
