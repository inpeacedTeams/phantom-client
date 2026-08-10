#pragma once
#include <imgui.h>
#include <vector>

#include "ios_theme.h"
#include "ios_widgets.h"
#include "../modules/module.h"

// =================================================================
// Settings view
// =================================================================
// Every module panel in the client is drawn by this file.
//
// It used to be that each module wrote its own panel in raw ImGui.
// The shell around them was a designed interface and the moment you
// opened a module it turned into a debug menu: grey combo boxes,
// square sliders, coloured text in whatever shade of red that
// particular file had picked. Worse, the two halves disagreed about
// behaviour. Some sliders let you type a value, some did not. Some
// clamped, some did not. Some settings were saved to the config and
// never drawn at all.
//
// None of that is fixable by tidying fifteen files, because there
// would still be fifteen files to keep in step. So the settings
// describe themselves and this is the only thing that draws them.
// A new module gets a correct, consistent, explained panel by
// binding its fields and writing no interface code whatsoever.
// =================================================================

namespace iOS {

// -----------------------------------------------------------------
// Callout
// -----------------------------------------------------------------
// The one piece of text a module is allowed to say for itself: a
// warning that a keybind could not be resolved, or that a mode is
// caught instantly by every prediction anticheat.
//
// Tinted plate, accent bar down the left, wrapped text. Three
// severities and no others, so "this is a warning" always looks the
// same rather than being whatever ImVec4 was to hand.
// -----------------------------------------------------------------
inline void Callout(Module::NoticeLevel level, const char* text) {
    if (!text || !text[0] || level == Module::NoticeLevel::None) return;

    ImU32 tint = Col::Blue;
    if (level == Module::NoticeLevel::Warning) tint = Col::Orange;
    if (level == Module::NoticeLevel::Danger)  tint = Col::Red;

    float w   = ImGui::GetContentRegionAvail().x;
    float pad = 11.0f * UI::scale;
    float bar = 3.0f * UI::scale;
    float wrap = w - pad * 2.0f - bar;

    Fonts::Push(Fonts::Caption);
    ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, wrap);
    Fonts::Pop(Fonts::Caption);

    float h = ts.y + pad * 2.0f;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 a = p;
    ImVec2 b(p.x + w, p.y + h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = 10.0f * UI::roundness;

    dl->AddRectFilled(a, b, Col::Alpha(tint, 0.10f), r);
    dl->AddRectFilled(a, ImVec2(a.x + bar, b.y), Col::Alpha(tint, 0.85f), r);

    Fonts::Push(Fonts::Caption);
    dl->AddText(Fonts::Caption, 0.0f,
                ImVec2(a.x + bar + pad, a.y + pad),
                Col::Label, text, nullptr, wrap);
    Fonts::Pop(Fonts::Caption);

    ImGui::Dummy(ImVec2(0, h + 6.0f));
}

// -----------------------------------------------------------------
// Visibility
// -----------------------------------------------------------------
// A row that does nothing in the current mode is not greyed out,
// it is absent. Greying it out still asks you to read it and work
// out why it will not move.
// -----------------------------------------------------------------
inline bool SettingVisible(const std::vector<Setting>& all, const Setting& s) {
    if (s.dependOn < 0 || s.dependOn >= (int)all.size()) return true;

    const Setting& gov = all[(size_t)s.dependOn];
    int v = (gov.type == Setting::Type::Bool)
              ? (gov.AsBool() ? 1 : 0)
              : gov.AsInt();

    return s.dependEqual ? (v == s.dependValue) : (v != s.dependValue);
}

// One control. Returns true when the value changed, which the
// caller uses only to know the config is dirty.
inline bool DrawSetting(Setting& s) {
    switch (s.type) {
        case Setting::Type::Bool:
            return SwitchRow(s.name.c_str(), (bool*)s.ptr, s.hint, Col::Blue);

        case Setting::Type::Int:
            return SliderRowInt(s.name.c_str(), (int*)s.ptr,
                                (int)s.lo, (int)s.hi, s.hint);

        case Setting::Type::Float:
            return SliderRow(s.name.c_str(), (float*)s.ptr,
                             s.lo, s.hi, s.fmt, s.hint);

        case Setting::Type::Mode: {
            // Segmented controls need room to breathe and read badly
            // squeezed between switch rows, so a mode always gets its
            // own card.
            ImGui::Dummy(ImVec2(0, 9.0f * UI::scale));
            ImGui::Indent(M::RowPadX);
            bool changed = Segmented(s.name.c_str(), s.options, s.optionCount,
                                     (int*)s.ptr,
                                     ImGui::GetContentRegionAvail().x - M::RowPadX);
            ImGui::Unindent(M::RowPadX);
            ImGui::Dummy(ImVec2(0, 11.0f * UI::scale));
            return changed;
        }
    }
    return false;
}

// -----------------------------------------------------------------
// Panel
// -----------------------------------------------------------------
// Walks the bound settings once and groups consecutive plain rows
// into a card, breaking out whenever a mode appears. That produces
// the iOS shape (a caption, a group, a caption, a group) without
// any module having to lay anything out.
// -----------------------------------------------------------------
inline bool RenderSettingGroup(std::vector<Setting>& all, bool advanced) {
    bool anyDrawn = false;
    bool cardOpen = false;
    bool firstInCard = true;

    auto CloseCard = [&]() {
        if (!cardOpen) return;
        EndCard();
        cardOpen = false;
        firstInCard = true;
    };

    for (auto& s : all) {
        if (s.advanced != advanced) continue;
        if (!SettingVisible(all, s)) continue;

        anyDrawn = true;

        if (s.type == Setting::Type::Mode) {
            CloseCard();
            SectionHeader(s.name.c_str());
            BeginCard();
            DrawSetting(s);
            EndCard();
            if (s.hint) Footnote(s.hint);
            continue;
        }

        if (!cardOpen) {
            BeginCard();
            cardOpen = true;
            firstInCard = true;
        }
        if (!firstInCard) RowSeparator();
        firstInCard = false;

        DrawSetting(s);
    }

    CloseCard();
    return anyDrawn;
}

// The everyday panel: the module's own notice, then its settings.
inline void RenderModuleSettings(Module& mod) {
    const char* text = nullptr;
    Module::NoticeLevel level = mod.Notice(&text);
    if (level != Module::NoticeLevel::None) {
        ImGui::Dummy(ImVec2(0, 2));
        Callout(level, text);
    }

    if (!RenderSettingGroup(mod.GetSettings(), false)) {
        Footnote("Nothing to configure. Turn it on and it works.");
    }
}

// Everything behind Advanced. Modules that have none never get the
// disclosure drawn in the first place, so this is only reached when
// there is something in it.
inline void RenderModuleAdvanced(Module& mod) {
    RenderSettingGroup(mod.GetSettings(), true);
}

} // namespace iOS
