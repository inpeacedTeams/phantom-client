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
// -----------------------------------------------------------------
// PLACEMENT
// -----------------------------------------------------------------
// This used to be a segmented control with four corners in it, and
// that is not a HUD layout, it is an apology for not having one.
// Every element now owns a NORMALISED anchor, 0..1 of the screen,
// and you place it by dragging it where you want it.
//
// Normalised rather than pixels because the game window gets
// resized, alt-tabbed and thrown onto a second monitor, and a HUD
// stored in pixels ends up off screen the first time any of that
// happens. Everything is additionally clamped into the safe margin
// each frame, so even a hand-edited config cannot hide it.
//
// The anchor is the corner NEAREST the screen edge it sits by:
//
//   anchor x > 0.5  the element is right aligned and grows left
//   anchor y > 0.5  the element is bottom anchored and grows up
//
// That one rule is why there is no "direction" setting. Drop the
// module list in the bottom right and new modules appear above the
// old ones, which is the only thing that could sensibly happen.
//
// ANIMATION
// Every pill owns its position and opacity. A module switched off
// slides toward the edge and fades, and only leaves the list once
// it has finished going, so nothing ever pops.
//
// COST
// Two allocations at startup and none afterwards: the item list is
// reused between frames, animation keys are addresses rather than
// built strings, and the whole thing early-outs to nothing when
// both elements are off.
// =================================================================

namespace iOS {

class HUD {
public:
    // Public so ConfigStore can persist them. A HUD you have to
    // rearrange after every inject is not a HUD anyone leaves on.
    inline static bool watermark  = true;
    inline static bool moduleList = true;
    inline static bool showFps    = true;

    // Normalised anchors. Defaults put the watermark in the top
    // right with the module list directly under it.
    inline static float wmX = 1.0f, wmY = 0.0f;
    inline static float mlX = 1.0f, mlY = 0.062f;

    static void ResetLayout() {
        wmX = 1.0f; wmY = 0.0f;
        mlX = 1.0f; mlY = 0.062f;
    }

    static void Reset() {
        watermark = moduleList = showFps = true;
        ResetLayout();
        s_pills.clear();
        s_editing = false;
        s_drag = -1;
    }

    // Configs written before the HUD could be dragged only stored a
    // corner index. Reading one has to place the elements somewhere
    // sensible rather than throwing the setting away, or upgrading
    // the client silently moves your HUD.
    static void ApplyCornerPreset(int corner) {
        const bool right  = (corner == 1 || corner == 3);
        const bool bottom = (corner == 2 || corner == 3);
        wmX = right  ? 1.0f : 0.0f;
        wmY = bottom ? 1.0f : 0.0f;
        mlX = wmX;
        mlY = bottom ? 0.938f : 0.062f;
    }

    // Read from the window procedure as well as the render thread.
    // A bool is a single aligned byte on every target this client
    // runs on, so the worst case is acting one frame late.
    static bool IsEditing() { return s_editing; }
    static void BeginEdit() { s_editing = true;  s_drag = -1; }
    static void EndEdit()   { s_editing = false; s_drag = -1; }

private:
    // ---- Metrics. Named, because a HUD full of loose numbers is
    // impossible to adjust without breaking the alignment. ----
    static constexpr float kMargin   = 14.0f;   // safe area from the edge
    static constexpr float kSnapPx   = 9.0f;    // how close before it snaps
    static constexpr float kBlockGap = 10.0f;   // between watermark and list
    static constexpr float kPillPadX = 12.0f;
    static constexpr float kPillPadY = 6.0f;
    static constexpr float kPillGap  = 6.0f;
    static constexpr float kDotR     = 3.5f;
    static constexpr float kSlideIn  = 26.0f;   // travel of an arriving pill

    struct Pill {
        float anim = 0.0f;      // 0 hidden, 1 fully in
        float y = 0.0f;         // animated vertical slot
        float width = 0.0f;     // remembered, so a leaving pill can still draw
        ImU32 color = 0;
        bool  seen = false;
    };

    inline static std::unordered_map<std::string, Pill> s_pills;
    inline static float s_fps = 0.0f;
    inline static char  s_fpsText[24] = {};

    // Reused between frames rather than rebuilt, which is the
    // difference between two allocations a frame and none.
    struct Item {
        const std::string* name;
        ImU32 color;
        float width;        // full pill width, not just the text
    };
    inline static std::vector<Item> s_items;
    inline static bool s_ghosts = false;   // showing placeholders, not modules

    // ---- Editor state ----
    inline static bool   s_editing = false;
    inline static int    s_drag = -1;
    inline static ImVec2 s_grab{ 0, 0 };
    inline static float  s_guideX = -1.0f;
    inline static float  s_guideY = -1.0f;

    static float Clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
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

    // ---------------------------------------------------------
    // Anchor <-> pixels
    // ---------------------------------------------------------
    // Resolve is the only place that decides which way an element
    // is aligned, and Store is its exact inverse, so dragging
    // something and letting go can never move it by a pixel.
    // ---------------------------------------------------------
    static ImVec2 Resolve(float ax, float ay, ImVec2 size, float sw, float sh) {
        const bool right = ax > 0.5f;
        const bool up    = ay > 0.5f;

        float x = right ? ax * sw - size.x : ax * sw;
        float y = up    ? ay * sh - size.y : ay * sh;

        float maxX = sw - kMargin - size.x;
        float maxY = sh - kMargin - size.y;
        if (maxX < kMargin) maxX = kMargin;   // element wider than the screen
        if (maxY < kMargin) maxY = kMargin;

        return ImVec2(Clamp(x, kMargin, maxX), Clamp(y, kMargin, maxY));
    }

    static void Store(ImVec2 pos, ImVec2 size, float sw, float sh,
                      float* ax, float* ay)
    {
        if (sw < 1.0f || sh < 1.0f) return;
        const bool right = (pos.x + size.x * 0.5f) > sw * 0.5f;
        const bool up    = (pos.y + size.y * 0.5f) > sh * 0.5f;
        *ax = (right ? pos.x + size.x : pos.x) / sw;
        *ay = (up    ? pos.y + size.y : pos.y) / sh;
    }

    // ---------------------------------------------------------
    // The card look: soft shadow, translucent white, light hairline
    // along the top edge.
    // ---------------------------------------------------------
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

    // ---------------------------------------------------------
    // Watermark
    // ---------------------------------------------------------
    static ImVec2 MeasureWatermark() {
        ImGuiIO& io = ImGui::GetIO();

        // Smoothed, or the number is an unreadable blur of digits
        s_fps += (io.Framerate - s_fps) * 0.06f;

        s_fpsText[0] = '\0';
        if (showFps) snprintf(s_fpsText, sizeof(s_fpsText), "%.0f FPS", s_fps);

        Fonts::Push(Fonts::BodyBold);
        ImVec2 ns = ImGui::CalcTextSize("Phantom");
        Fonts::Pop(Fonts::BodyBold);

        ImVec2 fs(0, 0);
        if (showFps) {
            Fonts::Push(Fonts::Caption);
            fs = ImGui::CalcTextSize(s_fpsText);
            Fonts::Pop(Fonts::Caption);
        }

        const float padX = 14.0f, padY = 9.0f;
        const float gap = showFps ? 14.0f : 0.0f;

        return ImVec2(padX * 2.0f + 8.0f + ns.x + gap + fs.x,
                      padY * 2.0f + ns.y);
    }

    static void DrawWatermark(ImDrawList* dl, ImVec2 a, ImVec2 size, float alpha) {
        ImVec2 b(a.x + size.x, a.y + size.y);
        GlassPanel(dl, a, b, size.y * 0.5f, alpha);

        const float padX = 14.0f;
        const float dotR = 4.0f;
        const float dotX = a.x + padX + dotR;
        const float cy   = a.y + size.y * 0.5f;

        dl->AddCircleFilled(ImVec2(dotX, cy), dotR, Col::Alpha(Col::Blue, alpha), 16);

        Fonts::Push(Fonts::BodyBold);
        ImVec2 ns = ImGui::CalcTextSize("Phantom");
        dl->AddText(ImVec2(dotX + dotR + 8.0f, cy - ns.y * 0.5f),
                    Col::Alpha(Col::Label, alpha), "Phantom");
        Fonts::Pop(Fonts::BodyBold);

        if (showFps) {
            Fonts::Push(Fonts::Caption);
            ImVec2 fs = ImGui::CalcTextSize(s_fpsText);
            dl->AddText(ImVec2(b.x - padX - fs.x, cy - fs.y * 0.5f),
                        Col::Alpha(Col::Label2, alpha), s_fpsText);
            Fonts::Pop(Fonts::Caption);
        }
    }

    // ---------------------------------------------------------
    // Module list
    // ---------------------------------------------------------
    // Builds the frame's items and returns the block size. In the
    // editor with nothing enabled it fills in three placeholders,
    // because you cannot position a list you cannot see.
    // ---------------------------------------------------------
    static ImVec2 MeasureList(bool editing, float* pillH, float* step) {
        static const std::string kGhost[3] = {
            std::string("Velocity"), std::string("AutoClicker"), std::string("ESP")
        };
        static const ImU32 kGhostCol[3] = { Col::Red, Col::Red, Col::Purple };

        s_items.clear();
        s_ghosts = false;

        Fonts::Push(Fonts::Body);

        for (auto& mod : ModuleManager::GetModules()) {
            if (!mod->IsEnabled()) continue;
            Item it;
            it.name  = &mod->GetName();
            it.color = CategoryColor(mod->GetCategory());
            it.width = ImGui::CalcTextSize(it.name->c_str()).x
                     + kPillPadX * 2.0f + 14.0f;
            s_items.push_back(it);
        }

        if (editing && s_items.empty()) {
            s_ghosts = true;
            for (int i = 0; i < 3; i++) {
                Item it;
                it.name  = &kGhost[i];
                it.color = kGhostCol[i];
                it.width = ImGui::CalcTextSize(kGhost[i].c_str()).x
                         + kPillPadX * 2.0f + 14.0f;
                s_items.push_back(it);
            }
        }

        const float lineH = ImGui::GetTextLineHeight();
        Fonts::Pop(Fonts::Body);

        // Widest first reads as a deliberate shape rather than a
        // ragged column.
        std::sort(s_items.begin(), s_items.end(),
                  [](const Item& a, const Item& b) { return a.width > b.width; });

        *pillH = lineH + kPillPadY * 2.0f;
        *step  = *pillH + kPillGap;

        float w = 0.0f;
        for (auto& it : s_items) if (it.width > w) w = it.width;

        float h = s_items.empty() ? 0.0f
                : (*step) * (float)s_items.size() - kPillGap;

        return ImVec2(w, h);
    }

    static void DrawPill(ImDrawList* dl, float anchorX, float y,
                         float w, float h, const char* text,
                         ImU32 accent, float t, bool rightAlign)
    {
        if (t < 0.01f) return;

        // Slides in from whichever edge it is anchored to
        const float slide = (1.0f - t) * kSlideIn;
        const float x = rightAlign ? (anchorX - w + slide) : (anchorX - slide);

        ImVec2 a(x, y);
        ImVec2 b(x + w, y + h);

        GlassPanel(dl, a, b, h * 0.5f, t);

        const float cy = a.y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(a.x + kPillPadX + kDotR - 2.0f, cy), kDotR,
                            Col::Alpha(accent, t), 14);

        Fonts::Push(Fonts::Body);
        ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(a.x + kPillPadX + kDotR * 2.0f + 6.0f, cy - ts.y * 0.5f),
                    Col::Alpha(Col::Label, t), text);
        Fonts::Pop(Fonts::Body);
    }

    static void DrawList(ImDrawList* dl, ImVec2 pos, ImVec2 size,
                         float pillH, float step, bool rightAlign, bool up,
                         float alpha)
    {
        const float anchorX = rightAlign ? pos.x + size.x : pos.x;

        // Placeholders never enter the animation store: they do not
        // belong to a module and would animate out the moment the
        // editor closed.
        if (s_ghosts) {
            for (size_t i = 0; i < s_items.size(); i++) {
                float y = pos.y + step * (float)i;
                DrawPill(dl, anchorX, y, s_items[i].width, pillH,
                         s_items[i].name->c_str(), s_items[i].color,
                         alpha * 0.5f, rightAlign);
            }
            return;
        }

        for (auto& kv : s_pills) kv.second.seen = false;

        const int n = (int)s_items.size();
        for (int i = 0; i < n; i++) {
            Item& it = s_items[i];
            Pill& p = s_pills[*it.name];
            const bool isNew = !p.seen && p.anim <= 0.001f;

            p.seen  = true;
            p.width = it.width;
            p.color = it.color;

            // Growing upward means the block is bottom anchored, so
            // index 0 sits at the bottom and the list pushes up.
            const float targetY = up
                ? pos.y + size.y - pillH - step * (float)i
                : pos.y + step * (float)i;

            if (isNew) p.y = targetY;   // appear in place, then slide in

            // Keyed by address rather than a built string: the old
            // version allocated two std::strings per pill per frame
            // purely to make an ImGui id.
            p.y    = Anim::To(ImGui::GetID((const void*)&p.y), targetY, 16.0f);
            p.anim = Anim::To(ImGui::GetID((const void*)&p.anim), 1.0f, 15.0f);
        }

        // Anything switched off animates out before it is dropped
        for (auto it = s_pills.begin(); it != s_pills.end(); ) {
            if (it->second.seen) { ++it; continue; }

            it->second.anim = Anim::To(
                ImGui::GetID((const void*)&it->second.anim), 0.0f, 15.0f);

            if (it->second.anim < 0.01f) { it = s_pills.erase(it); continue; }

            DrawPill(dl, anchorX, it->second.y, it->second.width, pillH,
                     it->first.c_str(), it->second.color,
                     it->second.anim * alpha, rightAlign);
            ++it;
        }

        for (auto& it : s_items) {
            Pill& p = s_pills[*it.name];
            DrawPill(dl, anchorX, p.y, it.width, pillH,
                     it.name->c_str(), it.color, p.anim * alpha, rightAlign);
        }
    }

    // =========================================================
    // Editor
    // =========================================================
    // Drag to place. Snaps to the safe margin on each edge, to the
    // centre lines, and to the edges of the other element so the two
    // line up without pixel hunting. A guide is drawn only while a
    // snap is actually holding, which is the only time it says
    // anything.
    //
    // Input is read straight off ImGuiIO rather than through widgets
    // because the HUD lives on the foreground draw list with no
    // window behind it. The menu sheet is hidden while this is open,
    // so nothing else is competing for the click.
    // =========================================================
    struct EditTarget {
        const char* label;
        ImVec2 pos;
        ImVec2 size;
        float* ax;
        float* ay;
        bool   live;
    };

    static bool Hit(ImVec2 m, ImVec2 a, ImVec2 size) {
        return m.x >= a.x && m.x <= a.x + size.x
            && m.y >= a.y && m.y <= a.y + size.y;
    }

    // Nearest candidate within the threshold, or the value untouched
    static float SnapTo(float v, const float* candidates, int count, float* guide) {
        float best = v, bestD = kSnapPx;
        int hit = -1;
        for (int i = 0; i < count; i++) {
            float d = std::fabs(v - candidates[i]);
            if (d < bestD) { bestD = d; best = candidates[i]; hit = i; }
        }
        if (hit >= 0 && guide) *guide = best;
        return best;
    }

    static void DragTargets(EditTarget* t, int count, float sw, float sh,
                            bool consumed)
    {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 m = io.MousePos;

        s_guideX = s_guideY = -1.0f;

        if (!io.MouseDown[0]) s_drag = -1;

        // Topmost first, so the element drawn last is grabbed first
        if (s_drag < 0 && io.MouseClicked[0] && !consumed) {
            for (int i = count - 1; i >= 0; i--) {
                if (!t[i].live) continue;
                if (!Hit(m, t[i].pos, t[i].size)) continue;
                s_drag = i;
                s_grab = ImVec2(m.x - t[i].pos.x, m.y - t[i].pos.y);
                break;
            }
        }

        if (s_drag < 0 || s_drag >= count || !t[s_drag].live) return;

        EditTarget& d = t[s_drag];
        ImVec2 p(m.x - s_grab.x, m.y - s_grab.y);

        // ---- Candidates ----
        float xs[6], ys[6];
        int nx = 0, ny = 0;
        xs[nx++] = kMargin;                         // left margin
        xs[nx++] = sw - kMargin - d.size.x;         // right margin
        xs[nx++] = (sw - d.size.x) * 0.5f;          // centred
        ys[ny++] = kMargin;
        ys[ny++] = sh - kMargin - d.size.y;
        ys[ny++] = (sh - d.size.y) * 0.5f;

        for (int i = 0; i < count; i++) {
            if (i == s_drag || !t[i].live) continue;
            xs[nx++] = t[i].pos.x;                                  // left edges
            xs[nx++] = t[i].pos.x + t[i].size.x - d.size.x;         // right edges
            ys[ny++] = t[i].pos.y + t[i].size.y + kBlockGap;        // stacked under
            ys[ny++] = t[i].pos.y - kBlockGap - d.size.y;           // stacked over
        }

        float gx = -1.0f, gy = -1.0f;
        p.x = SnapTo(p.x, xs, nx, &gx);
        p.y = SnapTo(p.y, ys, ny, &gy);

        float maxX = sw - kMargin - d.size.x;
        float maxY = sh - kMargin - d.size.y;
        if (maxX < kMargin) maxX = kMargin;
        if (maxY < kMargin) maxY = kMargin;
        p.x = Clamp(p.x, kMargin, maxX);
        p.y = Clamp(p.y, kMargin, maxY);

        if (gx >= 0.0f && std::fabs(p.x - gx) < 0.5f) s_guideX = p.x;
        if (gy >= 0.0f && std::fabs(p.y - gy) < 0.5f) s_guideY = p.y;

        d.pos = p;
        // Written live rather than on release: Store is the exact
        // inverse of Resolve, so the element cannot drift, and the
        // autosave picks it up even if the game dies mid-drag.
        Store(p, d.size, sw, sh, d.ax, d.ay);
    }

    // A button with no ImGui window behind it
    static bool RawButton(ImDrawList* dl, ImVec2 a, ImVec2 size,
                          const char* label, ImU32 col, bool filled,
                          float alpha, const char* key)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 b(a.x + size.x, a.y + size.y);

        const bool hov = Hit(io.MousePos, a, size);
        const float h = Anim::ToStr(key, hov ? 1.0f : 0.0f, 18.0f);
        const float r = size.y * 0.5f;

        if (filled) {
            if (h > 0.02f) Glow(dl, a, b, col, r, h * 0.7f * alpha, 4);
            dl->AddRectFilled(a, b, Col::Alpha(col, (0.9f + 0.1f * h) * alpha), r);
        } else {
            dl->AddRectFilled(a, b,
                Col::Alpha(IM_COL32(255, 255, 255, 255), (0.10f + 0.10f * h) * alpha), r);
            dl->AddRect(a, b,
                Col::Alpha(IM_COL32(255, 255, 255, 255), (0.30f + 0.25f * h) * alpha),
                r, 0, 1.0f);
        }

        Fonts::Push(Fonts::BodyBold);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(a.x + (size.x - ts.x) * 0.5f,
                           a.y + (size.y - ts.y) * 0.5f),
                    Col::Alpha(filled ? Col::OnAccent
                                      : IM_COL32(255, 255, 255, 255), alpha),
                    label);
        Fonts::Pop(Fonts::BodyBold);

        return hov && io.MouseClicked[0];
    }

    static void DrawOutline(ImDrawList* dl, ImVec2 a, ImVec2 size,
                            const char* label, bool active, float alpha)
    {
        ImVec2 b(a.x + size.x, a.y + size.y);
        const float r = 10.0f;
        const ImU32 c = Col::Alpha(Col::Blue, (active ? 1.0f : 0.55f) * alpha);

        dl->AddRect(ImVec2(a.x - 4.0f, a.y - 4.0f),
                    ImVec2(b.x + 4.0f, b.y + 4.0f), c, r, 0,
                    active ? 2.0f : 1.4f);

        // Corner ticks: reads as a handle without pretending to be a
        // resize grip, which this is not.
        const float t = 7.0f;
        const ImU32 cc = Col::Alpha(Col::Blue, alpha);
        dl->AddLine(ImVec2(a.x - 4.0f, a.y - 4.0f), ImVec2(a.x - 4.0f + t, a.y - 4.0f), cc, 2.0f);
        dl->AddLine(ImVec2(a.x - 4.0f, a.y - 4.0f), ImVec2(a.x - 4.0f, a.y - 4.0f + t), cc, 2.0f);
        dl->AddLine(ImVec2(b.x + 4.0f, b.y + 4.0f), ImVec2(b.x + 4.0f - t, b.y + 4.0f), cc, 2.0f);
        dl->AddLine(ImVec2(b.x + 4.0f, b.y + 4.0f), ImVec2(b.x + 4.0f, b.y + 4.0f - t), cc, 2.0f);

        Fonts::Push(Fonts::Caption);
        ImVec2 ts = ImGui::CalcTextSize(label);
        float ly = a.y - 4.0f - ts.y - 6.0f;
        if (ly < 2.0f) ly = b.y + 8.0f;   // no room above, put it under
        dl->AddRectFilled(ImVec2(a.x - 4.0f, ly - 2.0f),
                          ImVec2(a.x + ts.x + 10.0f, ly + ts.y + 2.0f),
                          Col::Alpha(Col::Blue, 0.9f * alpha), 5.0f);
        dl->AddText(ImVec2(a.x + 1.0f, ly),
                    Col::Alpha(Col::OnAccent, alpha), label);
        Fonts::Pop(Fonts::Caption);
    }

    static void DrawEditorChrome(ImDrawList* dl, float sw, float sh, float alpha) {
        // ---- Snap guides ----
        if (s_guideX >= 0.0f)
            dl->AddLine(ImVec2(s_guideX, 0), ImVec2(s_guideX, sh),
                        Col::Alpha(Col::Pink, 0.75f * alpha), 1.0f);
        if (s_guideY >= 0.0f)
            dl->AddLine(ImVec2(0, s_guideY), ImVec2(sw, s_guideY),
                        Col::Alpha(Col::Pink, 0.75f * alpha), 1.0f);

        // ---- Safe area ----
        dl->AddRect(ImVec2(kMargin, kMargin), ImVec2(sw - kMargin, sh - kMargin),
                    Col::Alpha(IM_COL32(255, 255, 255, 255), 0.10f * alpha),
                    0.0f, 0, 1.0f);

        // ---- Banner ----
        const char* title = "HUD Layout";
        const char* hint  = "Drag an element to place it. It snaps to the edges "
                            "and to the other card.";

        Fonts::Push(Fonts::Title);
        ImVec2 tS = ImGui::CalcTextSize(title);
        Fonts::Pop(Fonts::Title);
        Fonts::Push(Fonts::Caption);
        ImVec2 hS = ImGui::CalcTextSize(hint);
        Fonts::Pop(Fonts::Caption);

        const float btnH = 30.0f;
        const float padX = 22.0f, padY = 16.0f, gap = 16.0f;
        const float doneW = 78.0f, resetW = 108.0f;

        float bodyW = tS.x;
        if (hS.x > bodyW) bodyW = hS.x;
        float cw = padX * 2.0f + bodyW;
        float minW = padX * 2.0f + doneW + resetW + 10.0f;
        if (cw < minW) cw = minW;
        float ch = padY * 2.0f + tS.y + 6.0f + hS.y + 14.0f + btnH;

        ImVec2 a((sw - cw) * 0.5f, sh * 0.06f - (1.0f - alpha) * 12.0f);
        ImVec2 b(a.x + cw, a.y + ch);

        dl->AddRectFilled(ImVec2(a.x + 2, a.y + 6), ImVec2(b.x + 2, b.y + 10),
                          IM_COL32(0, 0, 0, (int)(70 * alpha)), 18.0f);
        dl->AddRectFilled(a, b, IM_COL32(22, 24, 30, (int)(238 * alpha)), 18.0f);
        dl->AddRect(a, b, Col::Alpha(Col::Blue, 0.35f * alpha), 18.0f, 0, 1.0f);

        float y = a.y + padY;

        Fonts::Push(Fonts::Title);
        dl->AddText(ImVec2(a.x + (cw - tS.x) * 0.5f, y),
                    Col::Alpha(IM_COL32(255, 255, 255, 255), alpha), title);
        Fonts::Pop(Fonts::Title);
        y += tS.y + 6.0f;

        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(a.x + (cw - hS.x) * 0.5f, y),
                    Col::Alpha(IM_COL32(255, 255, 255, 170), alpha), hint);
        Fonts::Pop(Fonts::Caption);
        y += hS.y + 14.0f;

        float bx = a.x + (cw - (doneW + resetW + 10.0f)) * 0.5f;

        if (RawButton(dl, ImVec2(bx, y), ImVec2(resetW, btnH),
                      "Reset", Col::Blue, false, alpha, "hudReset")) {
            ResetLayout();
        }
        if (RawButton(dl, ImVec2(bx + resetW + 10.0f, y), ImVec2(doneW, btnH),
                      "Done", Col::Blue, true, alpha, "hudDone")) {
            EndEdit();
        }
    }

    // True if the pointer is over the banner, so a click there does
    // not also grab whatever HUD element happens to be underneath.
    static bool OverChrome(float sw, float sh) {
        ImGuiIO& io = ImGui::GetIO();
        return io.MousePos.y < sh * 0.06f + 150.0f
            && std::fabs(io.MousePos.x - sw * 0.5f) < 260.0f;
    }

public:
    // ---------------------------------------------------------
    // Render
    // ---------------------------------------------------------
    static void Render() {
        ImGuiIO& io = ImGui::GetIO();
        const float sw = io.DisplaySize.x;
        const float sh = io.DisplaySize.y;
        if (sw < 2.0f || sh < 2.0f) return;

        const bool edit = s_editing;
        const float editFade = Anim::ToStr("hudEditFade", edit ? 1.0f : 0.0f, 15.0f);

        if (!edit && !watermark && !moduleList) {
            if (!s_pills.empty()) s_pills.clear();
            return;
        }

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        // ---- Measure ----
        const bool showWm = watermark || edit;
        const bool showMl = moduleList || edit;

        ImVec2 wmSize(0, 0);
        if (showWm) wmSize = MeasureWatermark();

        float pillH = 0.0f, step = 0.0f;
        ImVec2 mlSize(0, 0);
        if (showMl) mlSize = MeasureList(edit, &pillH, &step);

        // ---- Place ----
        EditTarget targets[2];
        targets[0] = { "Watermark", Resolve(wmX, wmY, wmSize, sw, sh), wmSize,
                       &wmX, &wmY, showWm && wmSize.x > 1.0f };
        targets[1] = { "Modules", Resolve(mlX, mlY, mlSize, sw, sh), mlSize,
                       &mlX, &mlY, showMl && mlSize.x > 1.0f };

        if (edit) {
            // Scrim first: same draw list, so order is depth. The
            // world goes quiet and the HUD is the only thing lit.
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh),
                              IM_COL32(8, 10, 14, (int)(150 * editFade)));
            DragTargets(targets, 2, sw, sh, OverChrome(sw, sh));
        }

        // ---- Draw ----
        // Disabled elements are still shown, at half strength, while
        // the editor is open: placing something you cannot see is
        // impossible, and hiding it would make the switch feel like
        // it deleted the element.
        if (showWm && wmSize.x > 1.0f) {
            float a = watermark ? 1.0f : 0.45f * editFade;
            DrawWatermark(dl, targets[0].pos, wmSize, a);
        }
        if (showMl && mlSize.x > 1.0f) {
            const bool right = (targets[1].pos.x + mlSize.x * 0.5f) > sw * 0.5f;
            const bool up    = (targets[1].pos.y + mlSize.y * 0.5f) > sh * 0.5f;
            float a = moduleList ? 1.0f : 0.45f * editFade;
            DrawList(dl, targets[1].pos, mlSize, pillH, step, right, up, a);
        }

        if (editFade > 0.01f) {
            for (int i = 0; i < 2; i++) {
                if (!targets[i].live) continue;
                DrawOutline(dl, targets[i].pos, targets[i].size,
                            targets[i].label, s_drag == i, editFade);
            }
            DrawEditorChrome(dl, sw, sh, editFade);
        }
    }

    // ---------------------------------------------------------
    // Settings
    // ---------------------------------------------------------
    static void RenderSettings() {
        SectionHeader("HUD");
        BeginCard();
        SwitchRow("Watermark", &watermark,
                  "The client name in the corner", Col::Blue);
        if (watermark) {
            RowSeparator();
            SwitchRow("Frame rate", &showFps,
                      "Shown beside the name", Col::Blue);
        }
        RowSeparator();
        SwitchRow("Module list", &moduleList,
                  "What is currently running", Col::Purple);
        EndCard();

        BeginCard();
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Indent(M::RowPadX);
        if (Button("Edit Layout",
                   ImGui::GetContentRegionAvail().x - M::RowPadX,
                   Col::Blue, true)) {
            BeginEdit();
        }
        ImGui::Unindent(M::RowPadX);
        ImGui::Dummy(ImVec2(0, 10));
        EndCard();

        Footnote("Editing hides this menu and lets you drag the cards "
                 "straight onto the screen. They snap to the edges and to "
                 "each other. Escape or Done when you are finished, and the "
                 "positions are saved with everything else.");
    }
};

} // namespace iOS
