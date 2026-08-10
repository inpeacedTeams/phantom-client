#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdio>

#include "ios_theme.h"
#include "ios_widgets.h"
#include "../modules/module_manager.h"

// =================================================================
// iOS HUD
// =================================================================
// Drawn straight onto the foreground draw list rather than through
// ImGui windows, because a window brings padding, a background and
// a scroll region that all have to be fought off anyway.
//
// The look is iOS notification cards: translucent white over the
// game, generous rounding, one hairline of light along the top edge
// to suggest thickness. No real blur, since that would mean a
// framebuffer grab every frame for a decorative effect.
//
// Everything animates in and out. A module switched off slides its
// pill right and fades, and only leaves the list once it is gone.
// =================================================================

namespace iOS {

class HUD {
private:
    struct Pill {
        float anim = 0.0f;      // 0 hidden, 1 fully in
        float y = 0.0f;         // animated vertical slot
        bool  seen = false;
    };

    inline static std::unordered_map<std::string, Pill> s_pills;

    // ---- Settings ----
    inline static bool  s_watermark   = true;
    inline static bool  s_arraylist   = true;
    inline static bool  s_targetCard  = true;
    inline static int   s_corner      = 1;   // 0 TL, 1 TR, 2 BL, 3 BR
    inline static float s_scale       = 1.0f;

    inline static float s_fps = 0.0f;

    static ImU32 CategoryColor(ModuleCategory c) {
        switch (c) {
            case ModuleCategory::COMBAT:   return Col::Red;
            case ModuleCategory::MOVEMENT: return Col::Blue;
            case ModuleCategory::VISUAL:   return Col::Purple;
            case ModuleCategory::PLAYER:   return Col::Green;
            default:                       return Col::Orange;
        }
    }

    // The card look: soft shadow, translucent white, light hairline
    // along the top edge.
    static void GlassPanel(ImDrawList* dl, ImVec2 a, ImVec2 b,
                           float rounding, float alpha)
    {
        dl->AddRectFilled(ImVec2(a.x + 1.0f, a.y + 3.0f),
                          ImVec2(b.x + 1.0f, b.y + 4.0f),
                          IM_COL32(0, 0, 0, (int)(34 * alpha)), rounding);

        dl->AddRectFilled(a, b,
                          IM_COL32(252, 252, 254, (int)(232 * alpha)), rounding);

        dl->AddLine(ImVec2(a.x + rounding * 0.6f, a.y + 0.5f),
                    ImVec2(b.x - rounding * 0.6f, a.y + 0.5f),
                    IM_COL32(255, 255, 255, (int)(190 * alpha)), 1.0f);

        dl->AddRect(a, b, IM_COL32(0, 0, 0, (int)(18 * alpha)), rounding, 0, 1.0f);
    }

    // ---------------------------------------------------------
    // Watermark
    // ---------------------------------------------------------
    static void DrawWatermark(ImDrawList* dl, float x, float y, bool rightAlign) {
        ImGuiIO& io = ImGui::GetIO();

        // Smoothed so the number is readable instead of flickering
        float inst = io.Framerate;
        s_fps += (inst - s_fps) * 0.06f;

        char fps[24];
        snprintf(fps, sizeof(fps), "%.0f FPS", s_fps);

        const char* name = "Phantom";

        Fonts::Push(Fonts::BodyBold);
        ImVec2 ns = ImGui::CalcTextSize(name);
        Fonts::Pop(Fonts::BodyBold);

        Fonts::Push(Fonts::Caption);
        ImVec2 fs = ImGui::CalcTextSize(fps);
        Fonts::Pop(Fonts::Caption);

        float padX = 14.0f, padY = 9.0f, gap = 10.0f;
        float w = padX * 2.0f + ns.x + gap + fs.x + 10.0f;
        float h = padY * 2.0f + ns.y;

        ImVec2 a(rightAlign ? x - w : x, y);
        ImVec2 b(a.x + w, a.y + h);

        GlassPanel(dl, a, b, h * 0.5f, 1.0f);

        // Accent dot, the app-icon stand-in
        float dotR = 4.0f;
        float dotX = a.x + padX + dotR;
        float cy   = a.y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(dotX, cy), dotR, Col::Blue, 16);

        Fonts::Push(Fonts::BodyBold);
        dl->AddText(ImVec2(dotX + dotR + 8.0f, cy - ns.y * 0.5f),
                    Col::Label, name);
        Fonts::Pop(Fonts::BodyBold);

        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(b.x - padX - fs.x, cy - fs.y * 0.5f),
                    Col::Label2, fps);
        Fonts::Pop(Fonts::Caption);
    }

    // ---------------------------------------------------------
    // Module pills
    // ---------------------------------------------------------
    static void DrawArrayList(ImDrawList* dl, float anchorX, float startY,
                              bool rightAlign, bool upward)
    {
        for (auto& kv : s_pills) kv.second.seen = false;

        // Widest first reads as a deliberate shape rather than a
        // ragged column.
        struct Item {
            std::string name;
            ImU32 color;
            float width;
        };
        std::vector<Item> items;

        Fonts::Push(Fonts::Body);
        for (auto& mod : ModuleManager::GetModules()) {
            if (!mod->IsEnabled()) continue;
            Item it;
            it.name  = mod->GetName();
            it.color = CategoryColor(mod->GetCategory());
            it.width = ImGui::CalcTextSize(it.name.c_str()).x;
            items.push_back(std::move(it));
        }
        Fonts::Pop(Fonts::Body);

        for (size_t i = 0; i < items.size(); i++)
            for (size_t j = i + 1; j < items.size(); j++)
                if (items[j].width > items[i].width) std::swap(items[i], items[j]);

        float lineH = ImGui::GetTextLineHeight();
        float padX = 12.0f, padY = 6.0f;
        float pillH = lineH + padY * 2.0f;
        float step  = pillH + 6.0f;

        // Animate each pill toward its slot
        int idx = 0;
        for (auto& it : items) {
            Pill& p = s_pills[it.name];
            p.seen = true;

            float targetY = startY + (upward ? -step * idx : step * idx);
            if (p.anim <= 0.001f) p.y = targetY;   // appear in place

            ImGuiID yid = ImGui::GetID(("hudY_" + it.name).c_str());
            ImGuiID aid = ImGui::GetID(("hudA_" + it.name).c_str());
            p.y    = Anim::To(yid, targetY, 16.0f);
            p.anim = Anim::To(aid, 1.0f, 15.0f);
            idx++;
        }

        // Anything no longer enabled animates out before it is dropped
        for (auto it = s_pills.begin(); it != s_pills.end(); ) {
            if (it->second.seen) { ++it; continue; }

            ImGuiID aid = ImGui::GetID(("hudA_" + it->first).c_str());
            it->second.anim = Anim::To(aid, 0.0f, 15.0f);

            if (it->second.anim < 0.01f) { it = s_pills.erase(it); continue; }

            float w = 0.0f;
            Fonts::Push(Fonts::Body);
            w = ImGui::CalcTextSize(it->first.c_str()).x;
            Fonts::Pop(Fonts::Body);

            DrawPill(dl, anchorX, it->second.y, w, pillH, padX,
                     it->first.c_str(), Col::Label3, it->second.anim,
                     rightAlign);
            ++it;
        }

        for (auto& it : items) {
            Pill& p = s_pills[it.name];
            DrawPill(dl, anchorX, p.y, it.width, pillH, padX,
                     it.name.c_str(), it.color, p.anim, rightAlign);
        }
    }

    static void DrawPill(ImDrawList* dl, float anchorX, float y,
                         float textW, float h, float padX,
                         const char* text, ImU32 accent,
                         float t, bool rightAlign)
    {
        if (t < 0.01f) return;

        float w = textW + padX * 2.0f + 14.0f;

        // Slide in from the edge it is anchored to
        float slide = (1.0f - t) * 26.0f;
        float x = rightAlign ? (anchorX - w + slide) : (anchorX - slide);

        ImVec2 a(x, y);
        ImVec2 b(x + w, y + h);

        GlassPanel(dl, a, b, h * 0.5f, t);

        float dotR = 3.5f;
        float cy = a.y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(a.x + padX + dotR - 2.0f, cy), dotR,
                            Col::Alpha(accent, t), 14);

        Fonts::Push(Fonts::Body);
        ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(a.x + padX + dotR * 2.0f + 6.0f, cy - ts.y * 0.5f),
                    Col::Alpha(Col::Label, t), text);
        Fonts::Pop(Fonts::Body);
    }

    // ---------------------------------------------------------
    // Target card, bottom centre
    // ---------------------------------------------------------
    static void DrawTargetCard(ImDrawList* dl, float cx, float bottomY) {
        auto esp = ModuleManager::GetESP();
        if (!esp) return;

        // Only surfaces while something is actually being tracked
        static float shown = 0.0f;
        bool have = esp->IsEnabled();
        shown = Anim::ToStr("hudTarget", have ? 1.0f : 0.0f, 13.0f);
        if (shown < 0.01f) return;

        float w = 210.0f, h = 62.0f;
        float lift = (1.0f - shown) * 18.0f;
        ImVec2 a(cx - w * 0.5f, bottomY - h + lift);
        ImVec2 b(a.x + w, a.y + h);

        GlassPanel(dl, a, b, 16.0f, shown);

        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(a.x + 14.0f, a.y + 10.0f),
                    Col::Alpha(Col::Label2, shown), "TRACKING");
        Fonts::Pop(Fonts::Caption);

        Fonts::Push(Fonts::BodyBold);
        dl->AddText(ImVec2(a.x + 14.0f, a.y + 28.0f),
                    Col::Alpha(Col::Label, shown), "Players in range");
        Fonts::Pop(Fonts::BodyBold);
    }

public:
    static void Render() {
        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.0f || sh < 2.0f) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        const float margin = 14.0f;
        bool rightAlign = (s_corner == 1 || s_corner == 3);
        bool bottom     = (s_corner == 2 || s_corner == 3);

        float x = rightAlign ? (sw - margin) : margin;
        float y = bottom ? (sh - margin) : margin;

        if (s_watermark && !bottom) {
            DrawWatermark(dl, x, y, rightAlign);
            y += 46.0f;
        } else if (s_watermark && bottom) {
            DrawWatermark(dl, x, y - 38.0f, rightAlign);
            y -= 46.0f;
        }

        if (s_arraylist) DrawArrayList(dl, x, y, rightAlign, bottom);
        if (s_targetCard) DrawTargetCard(dl, sw * 0.5f, sh - 18.0f);

        Anim::GarbageCollect();
    }

    static void RenderSettings() {
        SectionHeader("On screen");
        BeginCard();
        SwitchRow("Watermark", &s_watermark, nullptr, Col::Blue);
        RowSeparator();
        SwitchRow("Module list", &s_arraylist, nullptr, Col::Purple);
        RowSeparator();
        SwitchRow("Target card", &s_targetCard, nullptr, Col::Green);
        EndCard();

        SectionHeader("Position");
        BeginCard();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Indent(M::RowPadX);
        {
            const char* corners[] = { "Top L", "Top R", "Bot L", "Bot R" };
            Segmented("hudcorner", corners, 4, &s_corner,
                      ImGui::GetContentRegionAvail().x - M::RowPadX);
        }
        ImGui::Unindent(M::RowPadX);
        ImGui::Dummy(ImVec2(0, 10));
        EndCard();

        Footnote("The HUD draws over the game rather than in a window, "
                 "so it never steals your mouse.");
    }

    static void Reset() { s_pills.clear(); }
};

} // namespace iOS
