#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>
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
// Switch  (51x31, the real iOS metrics)
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

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;

    ImU32 off = Col::Fill;
    ImU32 on  = enabled ? Col::Green : Col::Alpha(Col::Green, 0.45f);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      Col::Mix(off, on, t), r);

    if (hovered && enabled && t < 0.5f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          Col::Alpha(Col::Label, 0.04f), r);
    }

    // Knob widens toward the direction of travel while held
    float pad    = 2.0f;
    float kd     = M::SwitchKnob;
    float grow   = press * 4.0f;
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
inline void RowSeparator(float insetLeft = M::RowPadX) {
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
    float h = subtitle ? 58.0f : M::RowHeight;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Tapping the label toggles too, the way an iOS row does
    ImGui::InvisibleButton("##row", ImVec2(w - M::SwitchW - M::RowPadX * 2.0f, h));
    bool rowClicked = ImGui::IsItemClicked();
    bool rowHeld    = ImGui::IsItemActive();

    float pressT = Anim::To(ImGui::GetID("##rowp"), rowHeld ? 1.0f : 0.0f, 20.0f);
    if (pressT > 0.01f) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          Col::Alpha(Col::CardPressed, pressT));
    }

    float textX = p.x + M::RowPadX;

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
// Disclosure row: label left, chevron that rotates open
// =================================================================
inline bool DisclosureRow(const char* label, bool* open,
                          const char* value = nullptr, ImU32 accent = 0)
{
    ImGui::PushID(label);

    float w = ImGui::GetContentRegionAvail().x;
    float h = M::RowHeight;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::InvisibleButton("##disc", ImVec2(w, h));
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *open = !*open;

    bool held = ImGui::IsItemActive();
    float pressT = Anim::To(ImGui::GetID("##dp"), held ? 1.0f : 0.0f, 20.0f);
    float openT  = Anim::To(ImGui::GetID("##do"), *open ? 1.0f : 0.0f, 16.0f);

    if (pressT > 0.01f) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          Col::Alpha(Col::CardPressed, pressT));
    }

    float textX = p.x + M::RowPadX;
    if (accent) {
        dl->AddCircleFilled(ImVec2(textX + 4.0f, p.y + h * 0.5f), 4.0f, accent, 14);
        textX += 18.0f;
    }

    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(textX, p.y + (h - ts.y) * 0.5f), Col::Label, label);

    float cx = p.x + w - M::RowPadX - 4.0f;
    float cy = p.y + h * 0.5f;
    float ang = openT * 1.5707963f;
    float ca = std::cos(ang), sa = std::sin(ang);
    auto rot = [&](float x, float y) {
        return ImVec2(cx + (x * ca - y * sa), cy + (x * sa + y * ca));
    };
    dl->AddLine(rot(-3.0f, -5.0f), rot(2.0f, 0.0f), Col::Label3, 2.0f);
    dl->AddLine(rot(2.0f, 0.0f), rot(-3.0f, 5.0f), Col::Label3, 2.0f);

    if (value && value[0]) {
        ImVec2 vs = ImGui::CalcTextSize(value);
        dl->AddText(ImVec2(cx - 14.0f - vs.x, p.y + (h - vs.y) * 0.5f),
                    Col::Label2, value);
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::PopID();
    return clicked;
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
// Slider with the value as a chip on the left of the track
// =================================================================
inline bool SliderRow(const char* label, float* v, float lo, float hi,
                      const char* fmt = "%.2f")
{
    ImGui::PushID(label);

    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float h = 46.0f;
    float trackW = w * 0.48f;
    float trackX = p.x + w - M::RowPadX - trackW;
    float trackY = p.y + h * 0.5f;

    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + M::RowPadX, p.y + (h - ts.y) * 0.5f),
                Col::Label, label);

    ImGui::SetCursorScreenPos(ImVec2(trackX, p.y));
    ImGui::InvisibleButton("##sl", ImVec2(trackW, h));

    bool active = ImGui::IsItemActive();
    bool changed = false;

    if (active) {
        float local = (ImGui::GetIO().MousePos.x - trackX) / trackW;
        if (local < 0.0f) local = 0.0f;
        if (local > 1.0f) local = 1.0f;
        float nv = lo + (hi - lo) * local;
        if (nv != *v) { *v = nv; changed = true; }
    }

    float pressT = Anim::To(ImGui::GetID("##slp"), active ? 1.0f : 0.0f, 20.0f);

    float frac = (hi > lo) ? (*v - lo) / (hi - lo) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    float th = 4.0f + pressT * 2.0f;
    dl->AddRectFilled(ImVec2(trackX, trackY - th * 0.5f),
                      ImVec2(trackX + trackW, trackY + th * 0.5f),
                      Col::Fill, th * 0.5f);
    dl->AddRectFilled(ImVec2(trackX, trackY - th * 0.5f),
                      ImVec2(trackX + trackW * frac, trackY + th * 0.5f),
                      Col::Blue, th * 0.5f);

    float kr = 11.0f + pressT * 1.5f;
    ImVec2 kc(trackX + trackW * frac, trackY);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + 1.5f), kr, IM_COL32(0, 0, 0, 30), 24);
    dl->AddCircleFilled(kc, kr, Col::Card, 24);

    char buf[32];
    snprintf(buf, sizeof(buf), fmt, *v);
    Fonts::Push(Fonts::Caption);
    ImVec2 vs = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(trackX - vs.x - 12.0f, p.y + (h - vs.y) * 0.5f),
                Col::Label2, buf);
    Fonts::Pop(Fonts::Caption);

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
    float h = 40.0f;

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

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (filled) {
        dl->AddRectFilled(a, b, Col::Alpha(tint, 1.0f - press * 0.15f), 11.0f);
    } else {
        dl->AddRectFilled(a, b, Col::Alpha(tint, 0.10f + hov * 0.06f), 11.0f);
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

} // namespace iOS
