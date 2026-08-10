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
// LAYOUT
// This used to be four corner presets shared by both elements, so
// the watermark and the module list were welded together and three
// quarters of the screen were unreachable. Now each element owns a
// normalised position and you drag it where you want it.
//
// Normalised, not pixels, because the game window gets resized and
// goes fullscreen. A HUD pinned at x=1600 vanishes the moment you
// drop to a smaller window; a HUD at x=0.98 does not.
//
// Alignment is DERIVED from the position rather than being another
// setting to get wrong: an element in the right half of the screen
// grows leftward, one in the bottom half grows upward. Drag it to
// the bottom right and the module list stacks the sensible way
// without being told.
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

    // Normalised anchors, 0..1 of the screen.
    inline static float wmX = 0.012f, wmY = 0.020f;
    inline static float mlX = 0.988f, mlY = 0.020f;

    static void ResetLayout() {
        wmX = 0.012f; wmY = 0.020f;
        mlX = 0.988f; mlY = 0.020f;
    }

    static void Reset() {
        watermark = moduleList = showFps = true;
        ResetLayout();
        s_pills.clear();
        s_editing = false;
        s_drag = Drag::None;
    }

    static void SetEditing(bool on) {
        if (!on) s_drag = Drag::None;
        s_editing = on;
    }
    static bool IsEditing()  { return s_editing; }
    static bool IsDragging() { return s_drag != Drag::None; }

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

    // ---- Editor ----
    enum class Drag { None, Watermark, ModuleList };
    inline static bool  s_editing = false;
    inline static Drag  s_drag = Drag::None;
    inline static ImVec2 s_grab{ 0, 0 };    // pointer offset inside the box

    // Last frame's bounds, used for hit testing and for the editor
    // outline. One frame behind, which at 60 FPS is 16ms and cannot
    // be perceived while dragging something with the mouse.
    struct Rect { ImVec2 a{ 0, 0 }, b{ 0, 0 }; bool valid = false; };
    inline static Rect s_wmRect, s_mlRect;

    // ---- Layout constants ----
    // Named, because "14.0f" appearing in nine places is how two of
    // them end up different.
    static constexpr float kEdgeMargin   = 14.0f;   // resting distance from a screen edge
    static constexpr float kSnapPixels   = 10.0f;   // how close counts as snapped
    static constexpr float kPillGap      = 6.0f;
    static constexpr float kPillPadX     = 12.0f;
    static constexpr float kPillPadY     = 6.0f;
    static constexpr float kCardPadX     = 14.0f;
    static constexpr float kCardPadY     = 9.0f;
    static constexpr float kSlideOut     = 26.0f;   // how far a leaving pill travels

    static float Clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

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
                         float textW, float h, const char* text,
                         ImU32 accent, float t, bool rightAlign)
    {
        if (t < 0.01f) return;

        float w = textW + kPillPadX * 2.0f + 14.0f;

        // Slides toward whichever edge it is anchored to
        float slide = (1.0f - t) * kSlideOut;
        float x = rightAlign ? (anchorX - w + slide) : (anchorX - slide);

        ImVec2 a(x, y);
        ImVec2 b(x + w, y + h);

        GlassPanel(dl, a, b, h * 0.5f, t);

        float dotR = 3.5f;
        float cy = a.y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(a.x + kPillPadX + dotR - 2.0f, cy), dotR,
                            Col::Alpha(accent, t), 14);

        Fonts::Push(Fonts::Body);
        ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(a.x + kPillPadX + dotR * 2.0f + 6.0f, cy - ts.y * 0.5f),
                    Col::Alpha(Col::Label, t), text);
        Fonts::Pop(Fonts::Body);
    }

    // ---------------------------------------------------------
    // Watermark
    // ---------------------------------------------------------
    // Returns its own height so the caller can hang it above the
    // anchor when it lives in the bottom half.
    static float MeasureWatermark(const char* fps, ImVec2* nameSize,
                                  ImVec2* fpsSize)
    {
        Fonts::Push(Fonts::BodyBold);
        *nameSize = ImGui::CalcTextSize("Phantom");
        Fonts::Pop(Fonts::BodyBold);

        *fpsSize = ImVec2(0, 0);
        if (showFps) {
            Fonts::Push(Fonts::Caption);
            *fpsSize = ImGui::CalcTextSize(fps);
            Fonts::Pop(Fonts::Caption);
        }
        return kCardPadY * 2.0f + nameSize->y;
    }

    static void DrawWatermark(ImDrawList* dl, float x, float y,
                              bool rightAlign, bool bottom)
    {
        ImGuiIO& io = ImGui::GetIO();

        // Smoothed, or the number is an unreadable blur of digits
        s_fps += (io.Framerate - s_fps) * 0.06f;

        char fps[24] = {};
        if (showFps) snprintf(fps, sizeof(fps), "%.0f FPS", s_fps);

        ImVec2 ns, fs;
        float h = MeasureWatermark(fps, &ns, &fs);

        float gap = showFps ? 14.0f : 0.0f;
        float w = kCardPadX * 2.0f + 8.0f + ns.x + gap + fs.x;

        ImVec2 a(rightAlign ? x - w : x, bottom ? y - h : y);
        ImVec2 b(a.x + w, a.y + h);

        s_wmRect = { a, b, true };

        GlassPanel(dl, a, b, h * 0.5f, 1.0f);

        float dotR = 4.0f;
        float dotX = a.x + kCardPadX + dotR;
        float cy   = a.y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(dotX, cy), dotR, Col::Blue, 16);

        Fonts::Push(Fonts::BodyBold);
        dl->AddText(ImVec2(dotX + dotR + 8.0f, cy - ns.y * 0.5f),
                    Col::Label, "Phantom");
        Fonts::Pop(Fonts::BodyBold);

        if (showFps) {
            Fonts::Push(Fonts::Caption);
            dl->AddText(ImVec2(b.x - kCardPadX - fs.x, cy - fs.y * 0.5f),
                        Col::Label2, fps);
            Fonts::Pop(Fonts::Caption);
        }
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
        // ragged column.
        std::sort(s_items.begin(), s_items.end(),
                  [](const Item& a, const Item& b) { return a.width > b.width; });

        float pillH = lineH + kPillPadY * 2.0f;
        float step  = pillH + kPillGap;

        // Bounds for the editor, so an empty list can still be
        // grabbed and placed before you turn anything on.
        float widest = 0.0f;
        for (auto& it : s_items)
            if (it.width > widest) widest = it.width;
        if (widest < 60.0f) widest = 60.0f;

        float boxW = widest + kPillPadX * 2.0f + 14.0f;
        float boxH = s_items.empty() ? pillH
                   : step * (float)s_items.size() - kPillGap;

        {
            float x0 = rightAlign ? anchorX - boxW : anchorX;
            float y0 = upward ? startY - boxH : startY;
            s_mlRect = { ImVec2(x0, y0), ImVec2(x0 + boxW, y0 + boxH), true };
        }

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

            DrawPill(dl, anchorX, it->second.y, it->second.width, pillH,
                     it->first.c_str(), it->second.color,
                     it->second.anim, rightAlign);
            ++it;
        }

        for (auto& it : s_items) {
            Pill& p = s_pills[*it.name];
            DrawPill(dl, anchorX, p.y, it.width, pillH,
                     it.name->c_str(), it.color, p.anim, rightAlign);
        }
    }

    // ---------------------------------------------------------
    // Editor
    // ---------------------------------------------------------
    // A dashed outline round each element, a label saying what it
    // is, and drag to move. Snapping to the screen edges and the
    // centre lines, with a guide drawn while a snap is active, so
    // lining two elements up does not become an exercise in nudging
    // one pixel at a time.
    // ---------------------------------------------------------
    static void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
        const float dash = 6.0f, gap = 4.0f;

        for (float x = a.x; x < b.x; x += dash + gap) {
            float x2 = x + dash; if (x2 > b.x) x2 = b.x;
            dl->AddLine(ImVec2(x, a.y), ImVec2(x2, a.y), col, 1.4f);
            dl->AddLine(ImVec2(x, b.y), ImVec2(x2, b.y), col, 1.4f);
        }
        for (float y = a.y; y < b.y; y += dash + gap) {
            float y2 = y + dash; if (y2 > b.y) y2 = b.y;
            dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, y2), col, 1.4f);
            dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, y2), col, 1.4f);
        }
    }

    static bool Inside(const Rect& r, ImVec2 p) {
        return r.valid && p.x >= r.a.x && p.x <= r.b.x
                       && p.y >= r.a.y && p.y <= r.b.y;
    }

    static void EditorChrome(ImDrawList* dl, const Rect& r, const char* label,
                             bool hot)
    {
        if (!r.valid) return;

        ImVec2 a(r.a.x - 5.0f, r.a.y - 5.0f);
        ImVec2 b(r.b.x + 5.0f, r.b.y + 5.0f);

        ImU32 col = hot ? Col::Blue : Col::Alpha(Col::Blue, 0.55f);
        if (hot) dl->AddRectFilled(a, b, Col::Alpha(Col::Blue, 0.10f), 8.0f);
        DashedRect(dl, a, b, col);

        Fonts::Push(Fonts::Caption);
        ImVec2 ts = ImGui::CalcTextSize(label);
        float ly = a.y - ts.y - 5.0f;
        if (ly < 2.0f) ly = b.y + 5.0f;   // no room above: put it below
        dl->AddRectFilled(ImVec2(a.x, ly - 2.0f),
                          ImVec2(a.x + ts.x + 10.0f, ly + ts.y + 2.0f),
                          Col::Alpha(Col::Blue, hot ? 0.95f : 0.7f), 4.0f);
        dl->AddText(ImVec2(a.x + 5.0f, ly), Col::OnAccent, label);
        Fonts::Pop(Fonts::Caption);
    }

    // Snap a normalised anchor to the edges and the centre lines.
    // Reports which guides fired so they can be drawn.
    static void SnapAnchor(float* nx, float* ny, float sw, float sh,
                           bool rightAlign, bool bottom,
                           bool* guideX, bool* guideY)
    {
        float px = *nx * sw;
        float py = *ny * sh;

        float leftEdge  = kEdgeMargin;
        float rightEdge = sw - kEdgeMargin;
        float topEdge   = kEdgeMargin;
        float botEdge   = sh - kEdgeMargin;

        *guideX = *guideY = false;

        if (!rightAlign && std::fabs(px - leftEdge)  < kSnapPixels) px = leftEdge;
        if ( rightAlign && std::fabs(px - rightEdge) < kSnapPixels) px = rightEdge;
        if (!bottom     && std::fabs(py - topEdge)   < kSnapPixels) py = topEdge;
        if ( bottom     && std::fabs(py - botEdge)   < kSnapPixels) py = botEdge;

        if (std::fabs(px - sw * 0.5f) < kSnapPixels) { px = sw * 0.5f; *guideX = true; }
        if (std::fabs(py - sh * 0.5f) < kSnapPixels) { py = sh * 0.5f; *guideY = true; }

        *nx = Clamp01(px / sw);
        *ny = Clamp01(py / sh);
    }

    static void RunEditor(ImDrawList* dl, float sw, float sh) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 m = io.MousePos;

        // A wash over the world so it is obvious the HUD is in a
        // different mode.
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, 40));

        bool hotWm = watermark  && Inside(s_wmRect, m);
        bool hotMl = moduleList && Inside(s_mlRect, m);

        // ---- Begin a drag ----
        // Only when the pointer is not over the panel, or moving the
        // menu would also drag whatever is behind it.
        if (s_drag == Drag::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !io.WantCaptureMouse)
        {
            if (hotMl) {
                s_drag = Drag::ModuleList;
                s_grab = ImVec2(m.x - mlX * sw, m.y - mlY * sh);
            } else if (hotWm) {
                s_drag = Drag::Watermark;
                s_grab = ImVec2(m.x - wmX * sw, m.y - wmY * sh);
            }
        }

        if (s_drag != Drag::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            s_drag = Drag::None;

        bool guideX = false, guideY = false;

        if (s_drag != Drag::None) {
            float* nx = (s_drag == Drag::Watermark) ? &wmX : &mlX;
            float* ny = (s_drag == Drag::Watermark) ? &wmY : &mlY;

            *nx = Clamp01((m.x - s_grab.x) / sw);
            *ny = Clamp01((m.y - s_grab.y) / sh);

            SnapAnchor(nx, ny, sw, sh, *nx > 0.5f, *ny > 0.5f, &guideX, &guideY);
        }

        // ---- Chrome ----
        if (watermark)
            EditorChrome(dl, s_wmRect, "Watermark",
                         hotWm || s_drag == Drag::Watermark);
        if (moduleList)
            EditorChrome(dl, s_mlRect, "Module list",
                         hotMl || s_drag == Drag::ModuleList);

        // ---- Guides ----
        ImU32 guide = Col::Alpha(Col::Orange, 0.9f);
        if (guideX) dl->AddLine(ImVec2(sw * 0.5f, 0), ImVec2(sw * 0.5f, sh), guide, 1.0f);
        if (guideY) dl->AddLine(ImVec2(0, sh * 0.5f), ImVec2(sw, sh * 0.5f), guide, 1.0f);

        // ---- Instruction ----
        Fonts::Push(Fonts::Caption);
        const char* tip = s_drag != Drag::None
            ? "Release to place"
            : "Drag an element to move it. It snaps to the edges and the centre.";
        ImVec2 ts = ImGui::CalcTextSize(tip);
        float bx = (sw - ts.x) * 0.5f;
        float by = sh - 46.0f;
        dl->AddRectFilled(ImVec2(bx - 14.0f, by - 8.0f),
                          ImVec2(bx + ts.x + 14.0f, by + ts.y + 8.0f),
                          IM_COL32(20, 22, 28, 225), 9.0f);
        dl->AddText(ImVec2(bx, by), IM_COL32(255, 255, 255, 235), tip);
        Fonts::Pop(Fonts::Caption);
    }

public:
    static void Render() {
        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.0f || sh < 2.0f) return;

        if (!watermark && !moduleList && !s_editing) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        // Alignment falls out of where the element is, so there is
        // no second setting that can disagree with the first.
        bool wmRight = wmX > 0.5f, wmBottom = wmY > 0.5f;
        bool mlRight = mlX > 0.5f, mlBottom = mlY > 0.5f;

        s_wmRect.valid = s_mlRect.valid = false;

        if (watermark)
            DrawWatermark(dl, wmX * sw, wmY * sh, wmRight, wmBottom);

        if (moduleList)
            DrawModuleList(dl, mlX * sw, mlY * sh, mlRight, mlBottom);

        if (s_editing) RunEditor(dl, sw, sh);
    }

    static void RenderSettings() {
        SectionHeader("On screen");
        BeginCard();
        SwitchRow("Watermark", &watermark,
                  "The client name and your frame rate", Col::Blue);
        if (watermark) {
            RowSeparator();
            SwitchRow("Frame rate", &showFps, nullptr, Col::Blue);
        }
        RowSeparator();
        SwitchRow("Module list", &moduleList,
                  "What is currently running", Col::Purple);
        EndCard();

        SectionHeader("Layout");
        BeginCard();
        bool editing = s_editing;
        if (SwitchRow("Move HUD", &editing,
                      "Drag the elements anywhere on screen", Col::Orange))
            SetEditing(editing);
        EndCard();

        ImGui::Dummy(ImVec2(0, 6));
        if (Button("Reset HUD Positions",
                   ImGui::GetContentRegionAvail().x - M::RowPadX,
                   Col::Label2, false)) {
            ResetLayout();
            SetEditing(false);
        }

        Footnote("Positions are stored as a fraction of the screen, so the "
                 "HUD stays where you put it when you resize the window or "
                 "go fullscreen. An element in the right half grows leftward "
                 "and one in the bottom half grows upward.");
    }
};

} // namespace iOS
