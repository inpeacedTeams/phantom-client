#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstdio>

#include "ios_theme.h"
#include "ios_widgets.h"
#include "../modules/module_manager.h"

// =================================================================
// HUD
// =================================================================
// Drawn straight onto the foreground draw list rather than through
// ImGui windows, because a window brings padding, a background and
// a scroll region that all have to be fought off anyway.
//
// The look is iOS notification cards: translucent white over the
// game, generous rounding, one hairline of light along the top edge
// to suggest thickness.
//
// WHAT IS ON IT
//   Watermark   the client name and your frame rate
//   Module list what is currently running
//
// There used to be a third card that said "ESP: Tracking players"
// whenever the ESP was on. It carried no information the module
// list did not already give you, and a panel that exists to state
// the obvious is worse than empty space, so it is gone.
//
// ANIMATION
// Every pill owns its position and opacity. A module switched off
// slides toward the edge and fades, and only leaves the list once
// it has finished going, so nothing ever pops.
// =================================================================

namespace iOS {

class HUD {
public:
    // Public so ConfigStore can persist them. A HUD you have to
    // rearrange after every inject is not a HUD anyone leaves on.
    inline static bool watermark = true;
    inline static bool moduleList = true;
    inline static bool showFps = true;
    inline static int  corner = 1;          // 0 TL, 1 TR, 2 BL, 3 BR

    static void Reset() {
        watermark = moduleList = showFps = true;
        corner = 1;
        s_pills.clear();
    }

private:
    struct Pill {
        float anim = 0.0f;      // 0 hidden, 1 fully in
        float y = 0.0f;         // animated vertical slot
        float width = 0.0f;     // remembered, so a leaving pill can still draw
        ImU32 color = 0;
        bool  seen = false;
    };

    inline static std::unordered_map<std::string, Pill> s_pills;
    inline static float s_fps = 0.0f;

    // Reused between frames rather than rebuilt, which is the
    // difference between two allocations a frame and none.
    struct Item {
        const std::string* name;
        ImU32 color;
        float width;
    };
    inline static std::vector<Item> s_items;

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
        if (alpha <= 0.0f) return;

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

    static void DrawPill(ImDrawList* dl, float anchorX, float y,
                         float textW, float h, float padX,
                         const char* text, ImU32 accent,
                         float t, bool rightAlign)
    {
        if (t < 0.01f) return;

        float w = textW + padX * 2.0f + 14.0f;

        // Slides toward whichever edge it is anchored to
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
    // Watermark
    // ---------------------------------------------------------
    static float DrawWatermark(ImDrawList* dl, float x, float y, bool rightAlign) {
        ImGuiIO& io = ImGui::GetIO();

        // Smoothed, or the number is an unreadable blur of digits
        s_fps += (io.Framerate - s_fps) * 0.06f;

        char fps[24] = {};
        if (showFps) snprintf(fps, sizeof(fps), "%.0f FPS", s_fps);

        const char* name = "Phantom";

        Fonts::Push(Fonts::BodyBold);
        ImVec2 ns = ImGui::CalcTextSize(name);
        Fonts::Pop(Fonts::BodyBold);

        ImVec2 fs(0, 0);
        if (showFps) {
            Fonts::Push(Fonts::Caption);
            fs = ImGui::CalcTextSize(fps);
            Fonts::Pop(Fonts::Caption);
        }

        float padX = 14.0f, padY = 9.0f, gap = showFps ? 14.0f : 0.0f;
        float w = padX * 2.0f + 8.0f + ns.x + gap + fs.x;
        float h = padY * 2.0f + ns.y;

        ImVec2 a(rightAlign ? x - w : x, y);
        ImVec2 b(a.x + w, a.y + h);

        GlassPanel(dl, a, b, h * 0.5f, 1.0f);

        float dotR = 4.0f;
        float dotX = a.x + padX + dotR;
        float cy   = a.y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(dotX, cy), dotR, Col::Blue, 16);

        Fonts::Push(Fonts::BodyBold);
        dl->AddText(ImVec2(dotX + dotR + 8.0f, cy - ns.y * 0.5f),
                    Col::Label, name);
        Fonts::Pop(Fonts::BodyBold);

        if (showFps) {
            Fonts::Push(Fonts::Caption);
            dl->AddText(ImVec2(b.x - padX - fs.x, cy - fs.y * 0.5f),
                        Col::Label2, fps);
            Fonts::Pop(Fonts::Caption);
        }

        return h;
    }

    // ---------------------------------------------------------
    // Module pills
    // ---------------------------------------------------------
    static void DrawModuleList(ImDrawList* dl, float anchorX, float startY,
                               bool rightAlign, bool upward)
    {
        for (auto& kv : s_pills) kv.second.seen = false;

        s_items.clear();

        Fonts::Push(Fonts::Body);
        for (auto& mod : ModuleManager::GetModules()) {
            if (!mod->IsEnabled()) continue;
            Item it;
            it.name  = &mod->GetName();
            it.color = CategoryColor(mod->GetCategory());
            it.width = ImGui::CalcTextSize(it.name->c_str()).x;
            s_items.push_back(it);
        }
        float lineH = ImGui::GetTextLineHeight();
        Fonts::Pop(Fonts::Body);

        // Widest first reads as a deliberate shape rather than a
        // ragged column. std::sort rather than the hand-written
        // bubble sort this used to run every single frame.
        std::sort(s_items.begin(), s_items.end(),
                  [](const Item& a, const Item& b) { return a.width > b.width; });

        float padX = 12.0f, padY = 6.0f;
        float pillH = lineH + padY * 2.0f;
        float step  = pillH + 6.0f;

        int idx = 0;
        for (auto& it : s_items) {
            Pill& p = s_pills[*it.name];
            bool isNew = !p.seen && p.anim <= 0.001f;

            p.seen  = true;
            p.width = it.width;
            p.color = it.color;

            float targetY = startY + (upward ? -step * (idx + 1) : step * idx);
            if (isNew) p.y = targetY;   // appear in place, then slide in

            p.y    = Anim::To(ImGui::GetID(("hudY_" + *it.name).c_str()),
                              targetY, 16.0f);
            p.anim = Anim::To(ImGui::GetID(("hudA_" + *it.name).c_str()),
                              1.0f, 15.0f);
            idx++;
        }

        // Anything switched off animates out before it is dropped
        for (auto it = s_pills.begin(); it != s_pills.end(); ) {
            if (it->second.seen) { ++it; continue; }

            it->second.anim = Anim::To(
                ImGui::GetID(("hudA_" + it->first).c_str()), 0.0f, 15.0f);

            if (it->second.anim < 0.01f) { it = s_pills.erase(it); continue; }

            DrawPill(dl, anchorX, it->second.y, it->second.width, pillH, padX,
                     it->first.c_str(), it->second.color,
                     it->second.anim, rightAlign);
            ++it;
        }

        for (auto& it : s_items) {
            Pill& p = s_pills[*it.name];
            DrawPill(dl, anchorX, p.y, it.width, pillH, padX,
                     it.name->c_str(), it.color, p.anim, rightAlign);
        }
    }

public:
    static void Render() {
        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.0f || sh < 2.0f) return;

        if (!watermark && !moduleList) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        const float margin = 14.0f;
        bool rightAlign = (corner == 1 || corner == 3);
        bool bottom     = (corner == 2 || corner == 3);

        float x = rightAlign ? (sw - margin) : margin;
        float y = bottom ? (sh - margin) : margin;

        if (watermark) {
            if (bottom) {
                float h = DrawWatermark(dl, x, y - 40.0f, rightAlign);
                y -= h + 10.0f;
            } else {
                float h = DrawWatermark(dl, x, y, rightAlign);
                y += h + 10.0f;
            }
        }

        if (moduleList) DrawModuleList(dl, x, y, rightAlign, bottom);
    }

    static void RenderSettings() {
        SectionHeader("On screen");
        BeginCard();
        SwitchRow("Watermark", &watermark,
                  "The client name in the corner", Col::Blue);
        if (watermark) {
            RowSeparator();
            SwitchRow("Frame rate", &showFps, nullptr, Col::Blue);
        }
        RowSeparator();
        SwitchRow("Module list", &moduleList,
                  "What is currently running", Col::Purple);
        EndCard();

        SectionHeader("Corner");
        BeginCard();
        ImGui::Dummy(ImVec2(0, 9));
        ImGui::Indent(M::RowPadX);
        {
            const char* corners[] = { "Top L", "Top R", "Bot L", "Bot R" };
            Segmented("hudcorner", corners, 4, &corner,
                      ImGui::GetContentRegionAvail().x - M::RowPadX);
        }
        ImGui::Unindent(M::RowPadX);
        ImGui::Dummy(ImVec2(0, 11));
        EndCard();

        Footnote("The HUD draws over the game rather than in a window, so it "
                 "never steals your mouse and costs nothing when both of "
                 "these are off.");
    }
};

} // namespace iOS
