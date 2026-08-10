#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ios_theme.h"

// =================================================================
// iOS widgets
// =================================================================
// Drawn by hand rather than restyled, because the shapes ImGui
// ships cannot become these. A switch is a pill with a shadowed
// knob that widens under the finger; a segmented control is a
// capsule that slides between labels. Neither exists in the stock
// widget set at any style setting.
//
// Every widget pulls its animation state from iOS::Anim keyed on
// the ImGui ID, so callers store nothing.
// =================================================================

namespace iOS {

// Shadow under a card, faked with two offset rounded rects
inline void DropShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
    dl->AddRectFilled(ImVec2(a.x, a.y + 2.0f), ImVec2(b.x, b.y + 3.0f),
                      Col::Shadow2, rounding);
    dl->AddRectFilled(ImVec2(a.x, a.y + 1.0f), ImVec2(b.x, b.y + 1.5f),
                      Col::Shadow1, rounding);
}

// =================================================================
// Glow
// =================================================================
// Concentric rounded rects, each one larger and fainter. Four
// layers is the point where adding more stops being visible, and
// four filled rects is nothing next to the world behind them.
//
// The alpha falls off quadratically rather than linearly, which is
// what makes it read as light rather than as an outline.
// =================================================================
inline void Glow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col,
                 float rounding, float strength = 1.0f, int layers = 4)
{
    if (!UI::glow || strength <= 0.001f) return;
    strength *= UI::glowAmount;
    if (strength <= 0.001f) return;

    for (int i = layers; i >= 1; i--) {
        float t = (float)i / (float)layers;
        float spread = 7.0f * t * UI::scale;
        float alpha  = 0.16f * strength * (1.0f - t) * (1.0f - t) * 2.4f;
        if (alpha <= 0.002f) continue;

        dl->AddRectFilled(ImVec2(a.x - spread, a.y - spread),
                          ImVec2(b.x + spread, b.y + spread),
                          Col::Alpha(col, alpha),
                          rounding + spread * 0.6f);
    }
}

inline void GlowCircle(ImDrawList* dl, ImVec2 c, float r, ImU32 col,
                       float strength = 1.0f, int layers = 4)
{
    if (!UI::glow || strength <= 0.001f) return;
    strength *= UI::glowAmount;
    if (strength <= 0.001f) return;

    for (int i = layers; i >= 1; i--) {
        float t = (float)i / (float)layers;
        float alpha = 0.18f * strength * (1.0f - t) * (1.0f - t) * 2.4f;
        if (alpha <= 0.002f) continue;
        dl->AddCircleFilled(c, r + 7.0f * t * UI::scale,
                            Col::Alpha(col, alpha), 20);
    }
}

// =================================================================
// Switch  (51x31 at 1x, the real iOS metrics)
// =================================================================
// Three things move: the track colour crossfades, the knob slides,
// and the knob stretches toward the direction of travel while your
// finger is down. The glow only exists in the on state and fades in
// with it, so turning something on reads as it lighting up.
// =================================================================
inline bool Switch(const char* id, bool* v, bool enabled = true) {
    ImGui::PushID(id);
    ImGuiID wid = ImGui::GetID("##sw");

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(M::SwitchW, M::SwitchH);

    ImGui::InvisibleButton("##sw", size);
    bool pressed = enabled && ImGui::IsItemClicked();
    if (pressed) *v = !*v;

    bool held    = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    float t     = Anim::To(wid, *v ? 1.0f : 0.0f, 16.0f);
    float press = Anim::To(ImGui::GetID("##swp"), held ? 1.0f : 0.0f, 22.0f);
    float hov   = Anim::To(ImGui::GetID("##swh"), hovered ? 1.0f : 0.0f, 16.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = pos;
    ImVec2 b(pos.x + size.x, pos.y + size.y);
    float r = size.y * 0.5f;

    ImU32 off = Col::Fill;
    ImU32 on  = enabled ? Col::Green : Col::Alpha(Col::Green, 0.45f);

    // Halo under the track, only while on
    if (t > 0.02f) Glow(dl, a, b, on, r, t * (0.55f + 0.45f * hov));

    dl->AddRectFilled(a, b, Col::Mix(off, on, t), r);

    if (hovered && enabled) {
        dl->AddRectFilled(a, b, Col::Alpha(Col::Label, 0.05f * hov * (1.0f - t)), r);
        dl->AddRect(a, b, Col::Alpha(Col::Label, 0.07f * hov), r, 0, 1.0f);
    }

    // Knob widens toward the direction of travel while held
    float pad    = 2.0f * UI::scale;
    float kd     = M::SwitchKnob;
    float grow   = press * 4.0f * UI::scale;
    float travel = size.x - kd - pad * 2.0f;
    float kx     = pos.x + pad + travel * t;
    if (t > 0.5f) kx -= grow;

    ImVec2 ka(kx, pos.y + pad);
    ImVec2 kb(kx + kd + grow, pos.y + size.y - pad);
    float kr = (kb.y - ka.y) * 0.5f;

    dl->AddRectFilled(ImVec2(ka.x, ka.y + 1.5f), ImVec2(kb.x, kb.y + 1.5f),
                      IM_COL32(0, 0, 0, 28), kr);
    dl->AddRectFilled(ka, kb, IM_COL32(255, 255, 255, 255), kr);

    ImGui::PopID();
    return pressed;
}

// =================================================================
// Segmented control
// =================================================================
inline bool Segmented(const char* id, const char* const items[], int count,
                      int* current, float width = -1.0f)
{
    ImGui::PushID(id);
    ImGuiID wid = ImGui::GetID("##seg");

    if (width <= 0.0f) width = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(width, M::SegHeight);

    ImGui::InvisibleButton("##seg", size);

    bool changed = false;
    float segW = size.x / (float)count;

    if (ImGui::IsItemClicked()) {
        float local = ImGui::GetIO().MousePos.x - pos.x;
        int idx = (int)(local / segW);
        if (idx < 0) idx = 0;
        if (idx >= count) idx = count - 1;
        if (idx != *current) { *current = idx; changed = true; }
    }

    bool held = ImGui::IsItemActive();
    float sel   = Anim::To(wid, (float)*current, 18.0f);
    float press = Anim::To(ImGui::GetID("##segp"), held ? 1.0f : 0.0f, 20.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      Col::Fill, M::SegRadius);

    float inset  = 2.0f;
    float shrink = press * 1.5f;
    ImVec2 ca(pos.x + inset + segW * sel + shrink, pos.y + inset + shrink);
    ImVec2 cb(ca.x + segW - inset * 2.0f - shrink * 2.0f,
              pos.y + size.y - inset - shrink);

    dl->AddRectFilled(ImVec2(ca.x, ca.y + 1.5f), ImVec2(cb.x, cb.y + 1.5f),
                      IM_COL32(0, 0, 0, 20), M::SegRadius - 1.0f);
    dl->AddRectFilled(ca, cb, Col::Card, M::SegRadius - 1.0f);

    // The selected label darkens as the capsule arrives under it
    for (int i = 0; i < count; i++) {
        ImVec2 ts = ImGui::CalcTextSize(items[i]);
        float cx = pos.x + segW * i + (segW - ts.x) * 0.5f;
        float cy = pos.y + (size.y - ts.y) * 0.5f;

        float d = std::fabs(sel - (float)i);
        if (d > 1.0f) d = 1.0f;
        dl->AddText(ImVec2(cx, cy), Col::Mix(Col::Label, Col::Label2, d), items[i]);
    }

    ImGui::PopID();
    return changed;
}

// =================================================================
// Card
// =================================================================
// The white rounded group an iOS settings screen is built from.
// The height is unknown until the content is laid out, so the
// background is drawn into a reserved draw-list channel and filled
// in at End.
// =================================================================
struct CardState {
    ImDrawListSplitter splitter;
    ImVec2 start;
    float  width = 0.0f;
    bool   active = false;
};

inline CardState g_card;

inline void BeginCard(float width = -1.0f) {
    if (width <= 0.0f) width = ImGui::GetContentRegionAvail().x;

    g_card.start  = ImGui::GetCursorScreenPos();
    g_card.width  = width;
    g_card.active = true;

    g_card.splitter.Split(ImGui::GetWindowDrawList(), 2);
    g_card.splitter.SetCurrentChannel(ImGui::GetWindowDrawList(), 1);

    ImGui::Dummy(ImVec2(0, 2));
}

inline void EndCard() {
    if (!g_card.active) return;

    ImGui::Dummy(ImVec2(0, 2));

    ImVec2 end = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    g_card.splitter.SetCurrentChannel(dl, 0);

    ImVec2 a = g_card.start;
    ImVec2 b(a.x + g_card.width, end.y);

    DropShadow(dl, a, b, M::CardRadius);
    dl->AddRectFilled(a, b, Col::Card, M::CardRadius);

    g_card.splitter.Merge(dl);
    g_card.active = false;

    ImGui::Dummy(ImVec2(0, 1));
}

// Uppercase grey caption above a card
inline void SectionHeader(const char* text) {
    ImGui::Dummy(ImVec2(0, 8));
    Fonts::Push(Fonts::Caption);

    char buf[128];
    size_t n = std::strlen(text);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++) {
        char ch = text[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    buf[n] = '\0';

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + M::RowPadX, p.y),
                                        Col::Label2, buf);
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() + 6.0f));

    Fonts::Pop(Fonts::Caption);
}

// Hairline between rows, inset from the left like iOS
inline void RowSeparator(float insetLeft = -1.0f) {
    if (insetLeft < 0.0f) insetLeft = M::RowPadX;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x + insetLeft, p.y), ImVec2(p.x + w, p.y),
        Col::Separator, 1.0f);
    ImGui::Dummy(ImVec2(0, 1));
}

// =================================================================
// Row with a switch on the right
// =================================================================
inline bool SwitchRow(const char* label, bool* v,
                      const char* subtitle = nullptr,
                      ImU32 accent = 0, const char* badge = nullptr)
{
    ImGui::PushID(label);

    float w = ImGui::GetContentRegionAvail().x;
    float h = subtitle ? 58.0f * UI::scale : M::RowHeight;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Tapping the label toggles too, the way an iOS row does
    ImGui::InvisibleButton("##row", ImVec2(w - M::SwitchW - M::RowPadX * 2.0f, h));
    bool rowClicked = ImGui::IsItemClicked();
    bool rowHeld    = ImGui::IsItemActive();
    bool rowHover   = ImGui::IsItemHovered();

    float hovT   = Anim::To(ImGui::GetID("##rowh"), rowHover ? 1.0f : 0.0f, 18.0f);
    float pressT = Anim::To(ImGui::GetID("##rowp"), rowHeld ? 1.0f : 0.0f, 20.0f);
    float wash = pressT > hovT * 0.4f ? pressT : hovT * 0.4f;
    if (wash > 0.01f) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          Col::Alpha(Col::CardPressed, wash));
    }

    float textX = p.x + M::RowPadX + (UI::rowNudge ? hovT * 3.0f : 0.0f);

    if (accent) {
        dl->AddCircleFilled(ImVec2(textX + 4.0f, p.y + h * 0.5f), 4.0f, accent, 14);
        textX += 18.0f;
    }

    if (subtitle) {
        dl->AddText(ImVec2(textX, p.y + 10.0f), Col::Label, label);
        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(textX, p.y + 32.0f), Col::Label2, subtitle);
        Fonts::Pop(Fonts::Caption);
    } else {
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(textX, p.y + (h - ts.y) * 0.5f), Col::Label, label);
    }

    if (badge && badge[0]) {
        Fonts::Push(Fonts::Caption);
        ImVec2 bs = ImGui::CalcTextSize(badge);
        float bw = bs.x + 12.0f;
        float bx = p.x + w - M::SwitchW - M::RowPadX - bw - 10.0f;
        float by = p.y + (h - 20.0f) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + 20.0f),
                          Col::Fill, 6.0f);
        dl->AddText(ImVec2(bx + 6.0f, by + (20.0f - bs.y) * 0.5f),
                    Col::Label2, badge);
        Fonts::Pop(Fonts::Caption);
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x + w - M::SwitchW - M::RowPadX,
                                     p.y + (h - M::SwitchH) * 0.5f));
    bool swChanged = Switch("sw", v);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));

    bool changed = swChanged;
    if (rowClicked && !swChanged) { *v = !*v; changed = true; }

    ImGui::PopID();
    return changed;
}

// =================================================================
// Animated expanding container
// =================================================================
// The content height is not known in advance, and a zero-height
// child does NOT auto-size in ImGui: zero means "take the rest of
// the window", which made an earlier version swallow the page.
//
// Instead the child is always at least a pixel tall and the real
// height is read back from the layout cursor. Clipped items still
// go through ItemSize, so CursorMaxPos is correct even when nothing
// was actually drawn.
// =================================================================
inline bool BeginCollapsible(const char* id, bool open) {
    ImGui::PushID(id);

    float measured = Anim::Value(ImGui::GetID("##m"));
    float target   = open ? measured : 0.0f;
    float h        = Anim::To(ImGui::GetID("##h"), target, 15.0f);

    // Fully closed and staying closed: draw nothing at all
    if (!open && h < 0.75f) {
        ImGui::PopID();
        return false;
    }

    // Opening with no measurement yet: one clipped pixel is enough
    // to lay the content out and learn its height.
    float childH = h < 1.0f ? 1.0f : h;

    ImGui::BeginChild("##col", ImVec2(0, childH),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoBackground);
    return true;
}

inline void EndCollapsible() {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    float content = win->DC.CursorMaxPos.y - win->DC.CursorStartPos.y + 6.0f;

    ImGui::EndChild();

    if (content > 1.0f) Anim::Set(ImGui::GetID("##m"), content);

    ImGui::PopID();
}

// =================================================================
// Slider
// =================================================================
//   Label            12.4   ----------o------
//
// Drag the track, or CLICK THE NUMBER and type one. The typing half
// matters more than it looks: a slider is a terrible way to enter
// "exactly 12", and a control that only drags makes you nudge at it
// for ten seconds.
//
// Only one slider can be in text mode at a time, so the edit state
// is a single file-scope id rather than a map.
// =================================================================
inline ImGuiID g_sliderEdit = 0;
inline char    g_sliderBuf[32] = {};

inline bool SliderRow(const char* label, float* v, float lo, float hi,
                      const char* fmt = "%.2f", const char* hint = nullptr)
{
    ImGui::PushID(label);

    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float h = (hint ? 58.0f : 46.0f) * UI::scale;
    float rowMid = p.y + (hint ? 20.0f * UI::scale : h * 0.5f);

    float trackW = w * 0.40f;
    float trackX = p.x + w - M::RowPadX - trackW;
    float trackY = rowMid;

    // ---- Label ----
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + M::RowPadX, rowMid - ts.y * 0.5f), Col::Label, label);

    if (hint) {
        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(p.x + M::RowPadX, rowMid + ts.y * 0.5f + 4.0f),
                    Col::Label2, hint);
        Fonts::Pop(Fonts::Caption);
    }

    bool changed = false;

    // ---- The value chip, which doubles as the text field ----
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, *v);

    Fonts::Push(Fonts::Caption);
    ImVec2 vs = ImGui::CalcTextSize(buf);
    Fonts::Pop(Fonts::Caption);

    float chipW = vs.x + 16.0f;
    if (chipW < 44.0f * UI::scale) chipW = 44.0f * UI::scale;
    float chipH = 22.0f * UI::scale;
    ImVec2 ca(trackX - chipW - 10.0f, rowMid - chipH * 0.5f);
    ImVec2 cb(ca.x + chipW, ca.y + chipH);

    ImGuiID editId = ImGui::GetID("##edit");
    bool editing = (g_sliderEdit == editId);

    if (!editing) {
        ImGui::SetCursorScreenPos(ca);
        ImGui::InvisibleButton("##chip", ImVec2(chipW, chipH));
        bool chipHover = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
            g_sliderEdit = editId;
            snprintf(g_sliderBuf, sizeof(g_sliderBuf), "%.4g", *v);
            editing = true;
        }

        float ch = Anim::To(ImGui::GetID("##chiph"), chipHover ? 1.0f : 0.0f, 18.0f);
        dl->AddRectFilled(ca, cb, Col::Mix(Col::Fill, Col::BlueSoft, ch), 6.0f);

        Fonts::Push(Fonts::Caption);
        dl->AddText(ImVec2(ca.x + (chipW - vs.x) * 0.5f, rowMid - vs.y * 0.5f),
                    Col::Mix(Col::Label2, Col::Blue, ch), buf);
        Fonts::Pop(Fonts::Caption);

        if (chipHover) ImGui::SetTooltip("Click to type a value");
    }

    if (editing) {
        ImGui::SetCursorScreenPos(ca);
        ImGui::PushItemWidth(chipW);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));

        // Focus the field on the frame it appears, and never again,
        // or it steals the caret back every time you click away.
        if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();

        bool submit = ImGui::InputText("##val", g_sliderBuf, sizeof(g_sliderBuf),
            ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_AutoSelectAll |
            ImGuiInputTextFlags_CharsDecimal);

        bool escaped = ImGui::IsKeyPressed(ImGuiKey_Escape);
        bool done = submit || escaped || ImGui::IsItemDeactivated();

        ImGui::PopStyleVar();
        ImGui::PopItemWidth();

        if (done) {
            // Escape leaves the value alone, and anything that will
            // not parse is ignored rather than snapping to zero.
            if (!escaped && g_sliderBuf[0]) {
                char* end = nullptr;
                float parsed = std::strtof(g_sliderBuf, &end);
                if (end && end != g_sliderBuf) {
                    if (parsed < lo) parsed = lo;
                    if (parsed > hi) parsed = hi;
                    if (parsed != *v) { *v = parsed; changed = true; }
                }
            }
            g_sliderEdit = 0;
        }
    }

    // ---- Track ----
    ImGui::SetCursorScreenPos(ImVec2(trackX, p.y));
    ImGui::InvisibleButton("##sl", ImVec2(trackW, h));

    bool active = ImGui::IsItemActive();
    bool hover  = ImGui::IsItemHovered();

    if (active && !editing) {
        float local = (ImGui::GetIO().MousePos.x - trackX) / trackW;
        if (local < 0.0f) local = 0.0f;
        if (local > 1.0f) local = 1.0f;
        float nv = lo + (hi - lo) * local;
        if (nv != *v) { *v = nv; changed = true; }
    }

    float pressT = Anim::To(ImGui::GetID("##slp"), active ? 1.0f : 0.0f, 20.0f);
    float hovT   = Anim::To(ImGui::GetID("##slh"),
                            (hover || active) ? 1.0f : 0.0f, 18.0f);

    float frac = (hi > lo) ? (*v - lo) / (hi - lo) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // The fill eases toward the value, so a typed number slides into
    // place instead of teleporting.
    float shown = Anim::To(ImGui::GetID("##slv"), frac, active ? 45.0f : 16.0f);

    float th = (4.0f + pressT * 2.0f) * UI::scale;
    ImVec2 ta(trackX, trackY - th * 0.5f);
    ImVec2 tb(trackX + trackW, trackY + th * 0.5f);
    dl->AddRectFilled(ta, tb, Col::Fill, th * 0.5f);

    if (shown > 0.001f) {
        ImVec2 fb(trackX + trackW * shown, tb.y);
        Glow(dl, ta, fb, Col::Blue, th * 0.5f, (0.3f + 0.7f * hovT) * 0.8f, 3);
        dl->AddRectFilled(ta, fb, Col::Blue, th * 0.5f);
    }

    float kr = (10.0f + pressT * 2.0f + hovT) * UI::scale;
    ImVec2 kc(trackX + trackW * shown, trackY);
    GlowCircle(dl, kc, kr, Col::Blue, hovT * 0.7f, 3);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + 1.5f), kr, IM_COL32(0, 0, 0, 30), 24);
    dl->AddCircleFilled(kc, kr, Col::Card, 24);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::PopID();
    return changed;
}

// Integer flavour. Rounds on the way out so the caller never sees
// 11.999 where it asked for a whole number.
inline bool SliderRowInt(const char* label, int* v, int lo, int hi,
                         const char* hint = nullptr)
{
    float f = (float)*v;
    if (!SliderRow(label, &f, (float)lo, (float)hi, "%.0f", hint)) return false;

    int nv = (int)(f + (f < 0 ? -0.5f : 0.5f));
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    if (nv == *v) return false;
    *v = nv;
    return true;
}

// =================================================================
// Search field
// =================================================================
inline bool SearchField(const char* id, char* buf, size_t bufSize,
                        float width = -1.0f,
                        const char* placeholder = "Search modules...")
{
    ImGui::PushID(id);

    if (width <= 0.0f) width = ImGui::GetContentRegionAvail().x;
    float h = 30.0f * UI::scale;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    bool has = buf[0] != '\0';

    // The plate is ours; the ImGui input on top of it is made
    // transparent so only our shape shows.
    ImGuiID fieldId = ImGui::GetID("##field");
    bool activeField = (ImGui::GetActiveID() == fieldId);
    float act = Anim::To(ImGui::GetID("##sfa"),
                         (activeField || has) ? 1.0f : 0.0f, 18.0f);

    ImVec2 a = p;
    ImVec2 b(p.x + width, p.y + h);
    float r = h * 0.5f;

    if (act > 0.02f) Glow(dl, a, b, Col::Blue, r, act * 0.5f, 3);
    dl->AddRectFilled(a, b, Col::Mix(Col::Fill, Col::BlueSoft, act), r);

    // Magnifier, drawn rather than shipped as a glyph so it does not
    // depend on a font that may not be installed.
    float gx = p.x + 14.0f * UI::scale;
    float gy = p.y + h * 0.5f - 1.0f;
    float gr = 4.5f * UI::scale;
    ImU32 gc = Col::Mix(Col::Label3, Col::Blue, act);
    dl->AddCircle(ImVec2(gx, gy), gr, gc, 12, 1.6f);
    dl->AddLine(ImVec2(gx + gr * 0.7f, gy + gr * 0.7f),
                ImVec2(gx + gr * 1.8f, gy + gr * 1.8f), gc, 1.6f);

    float textX = gx + gr + 12.0f * UI::scale;

    ImGui::SetCursorScreenPos(
        ImVec2(textX, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::PushItemWidth(width - (textX - p.x) - 30.0f * UI::scale);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));

    bool changed = ImGui::InputText("##field", buf, bufSize);

    ImGui::PopStyleColor(3);
    ImGui::PopItemWidth();

    if (!has && !activeField) {
        dl->AddText(ImVec2(textX + 2.0f,
                           p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                    Col::Label3, placeholder);
    }

    // Clear button, only once there is something to clear
    if (has) {
        float cx = p.x + width - 16.0f * UI::scale;
        float cy = p.y + h * 0.5f;
        float cr = 8.0f * UI::scale;

        ImGui::SetCursorScreenPos(ImVec2(cx - cr, cy - cr));
        ImGui::InvisibleButton("##clr", ImVec2(cr * 2.0f, cr * 2.0f));
        bool ch = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) { buf[0] = '\0'; changed = true; }

        dl->AddCircleFilled(ImVec2(cx, cy), cr,
            Col::Alpha(Col::Label2, ch ? 0.55f : 0.35f), 16);
        float k = cr * 0.42f;
        dl->AddLine(ImVec2(cx - k, cy - k), ImVec2(cx + k, cy + k), Col::Card, 1.6f);
        dl->AddLine(ImVec2(cx + k, cy - k), ImVec2(cx - k, cy + k), Col::Card, 1.6f);
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::PopID();
    return changed;
}

// =================================================================
// Filled pill button
// =================================================================
inline bool Button(const char* label, float width = -1.0f,
                   ImU32 tint = 0, bool filled = true)
{
    ImGui::PushID(label);

    if (tint == 0) tint = Col::Blue;
    if (width <= 0.0f) width = ImGui::GetContentRegionAvail().x;

    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 40.0f * UI::scale;

    ImGui::InvisibleButton("##btn", ImVec2(width, h));
    bool clicked = ImGui::IsItemClicked();
    bool held    = ImGui::IsItemActive();
    bool hover   = ImGui::IsItemHovered();

    float press = Anim::To(ImGui::GetID("##bp"), held ? 1.0f : 0.0f, 24.0f);
    float hov   = Anim::To(ImGui::GetID("##bh"), hover ? 1.0f : 0.0f, 14.0f);

    // Shrinks very slightly under the finger
    float s = press * 1.5f;
    ImVec2 a(p.x + s, p.y + s);
    ImVec2 b(p.x + width - s, p.y + h - s);
    float round = 11.0f * UI::roundness;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (filled) {
        Glow(dl, a, b, tint, round, hov * 0.75f, 3);
        dl->AddRectFilled(a, b, Col::Alpha(tint, 1.0f - press * 0.15f), round);
    } else {
        dl->AddRectFilled(a, b, Col::Alpha(tint, 0.10f + hov * 0.08f), round);
    }

    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f,
                       a.y + (b.y - a.y - ts.y) * 0.5f),
                filled ? Col::OnAccent : tint, label);

    ImGui::PopID();
    return clicked;
}

// Plain informational row: label left, value right
inline void ValueRow(const char* label, const char* value) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = M::RowHeight;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + M::RowPadX, p.y + (h - ls.y) * 0.5f),
                Col::Label, label);

    ImVec2 vs = ImGui::CalcTextSize(value);
    dl->AddText(ImVec2(p.x + w - M::RowPadX - vs.x, p.y + (h - vs.y) * 0.5f),
                Col::Label2, value);

    ImGui::Dummy(ImVec2(0, h));
}

// Grey explanatory text under a card
inline void Footnote(const char* text) {
    Fonts::Push(Fonts::Caption);
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Indent(M::RowPadX);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Col::Label2));
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 4.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(M::RowPadX);
    Fonts::Pop(Fonts::Caption);
}

// =================================================================
// Hover info card
// =================================================================
// Floats beside the panel and explains whatever the pointer is on.
// Drawn on the foreground list so the window it describes cannot
// clip it, and given its own fade so moving between rows crossfades
// instead of blinking.
// =================================================================
inline void HoverCard(ImVec2 anchor, const char* title, const char* body,
                      float alpha, float maxWidth = 240.0f)
{
    if (alpha < 0.02f || !title || !title[0]) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    maxWidth *= UI::scale;
    float pad = 12.0f * UI::scale;
    float wrap = maxWidth - pad * 2.0f;

    Fonts::Push(Fonts::BodyBold);
    ImVec2 ts = ImGui::CalcTextSize(title);
    Fonts::Pop(Fonts::BodyBold);

    ImVec2 bs(0, 0);
    if (body && body[0]) {
        Fonts::Push(Fonts::Caption);
        bs = ImGui::CalcTextSize(body, nullptr, false, wrap);
        Fonts::Pop(Fonts::Caption);
    }

    float w = (ts.x > bs.x ? ts.x : bs.x) + pad * 2.0f;
    if (w > maxWidth) w = maxWidth;
    float h = pad * 2.0f + ts.y + (bs.y > 0 ? bs.y + 6.0f : 0.0f);

    // Slides in as it fades, which is what makes it feel attached to
    // the row rather than dropped onto the screen.
    ImVec2 a(anchor.x - (1.0f - alpha) * 8.0f, anchor.y);
    ImVec2 b(a.x + w, a.y + h);

    // Keep it on screen when the panel is dragged to the right edge
    ImGuiIO& io = ImGui::GetIO();
    if (b.x > io.DisplaySize.x - 8.0f) {
        float shift = b.x - (io.DisplaySize.x - 8.0f);
        a.x -= shift; b.x -= shift;
    }

    dl->AddRectFilled(ImVec2(a.x + 2, a.y + 4), ImVec2(b.x + 2, b.y + 5),
                      IM_COL32(0, 0, 0, (int)(40 * alpha)), M::CardRadius);
    dl->AddRectFilled(a, b, Col::Alpha(Col::Card, alpha), M::CardRadius);

    // A thin accent edge on the left, tying it to the row it belongs to
    dl->AddRectFilled(a, ImVec2(a.x + 3.0f * UI::scale, b.y),
                      Col::Alpha(Col::Blue, alpha), M::CardRadius);

    Fonts::Push(Fonts::BodyBold);
    dl->AddText(ImVec2(a.x + pad, a.y + pad),
                Col::Alpha(Col::Label, alpha), title);
    Fonts::Pop(Fonts::BodyBold);

    if (bs.y > 0) {
        Fonts::Push(Fonts::Caption);
        dl->AddText(Fonts::Caption, 0.0f,
                    ImVec2(a.x + pad, a.y + pad + ts.y + 6.0f),
                    Col::Alpha(Col::Label2, alpha), body,
                    nullptr, wrap);
        Fonts::Pop(Fonts::Caption);
    }
}

} // namespace iOS
