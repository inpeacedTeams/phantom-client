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
// behind, white cards on top, blue for anything interactive, green
// only for a switch that is on. Nothing else is coloured, which is
// what makes an iOS screen feel calm.
// =================================================================

namespace iOS {

// ---- Palette (iOS light) ----
namespace Col {
    // Surfaces
    inline const ImU32 GroupedBg   = IM_COL32(242, 242, 247, 255);  // behind cards
    inline const ImU32 Card        = IM_COL32(255, 255, 255, 255);
    inline const ImU32 CardPressed = IM_COL32(229, 229, 234, 255);
    inline const ImU32 Fill        = IM_COL32(120, 120, 128, 51);   // track, chips
    inline const ImU32 FillThick   = IM_COL32(120, 120, 128, 92);
    inline const ImU32 Separator   = IM_COL32(198, 198, 200, 140);

    // Text
    inline const ImU32 Label       = IM_COL32(0, 0, 0, 255);
    inline const ImU32 Label2      = IM_COL32(60, 60, 67, 153);     // 60%
    inline const ImU32 Label3      = IM_COL32(60, 60, 67, 76);      // 30%
    inline const ImU32 OnAccent    = IM_COL32(255, 255, 255, 255);

    // Accents
    inline const ImU32 Blue        = IM_COL32(0, 122, 255, 255);
    inline const ImU32 BlueSoft    = IM_COL32(0, 122, 255, 38);
    inline const ImU32 Green       = IM_COL32(52, 199, 89, 255);
    inline const ImU32 Red         = IM_COL32(255, 59, 48, 255);
    inline const ImU32 Orange      = IM_COL32(255, 149, 0, 255);
    inline const ImU32 Yellow      = IM_COL32(255, 204, 0, 255);
    inline const ImU32 Purple      = IM_COL32(175, 82, 222, 255);
    inline const ImU32 Teal        = IM_COL32(48, 176, 199, 255);
    inline const ImU32 Pink        = IM_COL32(255, 45, 85, 255);

    // Shadow, layered rather than blurred
    inline const ImU32 Shadow1     = IM_COL32(0, 0, 0, 10);
    inline const ImU32 Shadow2     = IM_COL32(0, 0, 0, 6);

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

// ---- Metrics ----
namespace M {
    inline constexpr float CardRadius   = 12.0f;
    inline constexpr float RowHeight    = 44.0f;   // the iOS row height
    inline constexpr float RowPadX      = 16.0f;
    inline constexpr float CardGap      = 22.0f;
    inline constexpr float SwitchW      = 51.0f;   // exact iOS switch
    inline constexpr float SwitchH      = 31.0f;
    inline constexpr float SwitchKnob   = 27.0f;
    inline constexpr float SegHeight    = 32.0f;
    inline constexpr float SegRadius    = 8.0f;
}

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
    static float To(ImGuiID id, float target, float speed = 14.0f) {
        ImGuiIO& io = ImGui::GetIO();
        Entry& e = s_store[id];

        if (e.frame == 0) e.value = target;   // no animation on first sight
        e.frame = ImGui::GetFrameCount();

        float dt = io.DeltaTime;
        if (dt > 0.1f) dt = 0.1f;             // survive a stall without a jump

        e.value += (target - e.value) * (1.0f - std::exp(-speed * dt));

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
    inline static ImFont* Title    = nullptr;   // 22px
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

        // Cyrillic too: module names are English but profile text
        // and anything the user adds may not be.
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
// language: rounded, roomy, blue accent, no borders.
// =================================================================

inline void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 20.0f;
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
