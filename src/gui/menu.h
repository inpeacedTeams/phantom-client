#pragma once
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "ios_theme.h"
#include "ios_widgets.h"
#include "ios_hud.h"
#include "../modules/module_manager.h"
#include "../render/backtrack_vis.h"
#include "../config/profiles.h"

// =================================================================
// Menu
// =================================================================
// An iOS Settings screen: grouped white cards on grey, one switch
// per row, a segmented control instead of tabs.
//
// Runs entirely on the render thread and must never call JNI.
// Toggles and profile loads are queued to ModuleManager and applied
// on the client thread where a valid JNIEnv exists.
//
// OPTIMISTIC SWITCHES
// Because a toggle is queued, IsEnabled() still reads the old value
// for up to one tick. Animating from that would make every switch
// stutter, so the pending value is held locally and shown
// immediately, then dropped once the real state agrees.
// =================================================================

class Menu {
private:
    inline static int  s_tab = 0;
    inline static bool s_open = false;
    inline static float s_fade = 0.0f;

    inline static std::unordered_map<std::string, bool> s_pending;
    inline static std::unordered_map<std::string, bool> s_expanded;

    static constexpr int kTabCount = 6;
    inline static const char* s_tabNames[kTabCount] = {
        "Combat", "Move", "Visual", "Player", "Misc", "Config"
    };
    inline static ModuleCategory s_tabCats[5] = {
        ModuleCategory::COMBAT,
        ModuleCategory::MOVEMENT,
        ModuleCategory::VISUAL,
        ModuleCategory::PLAYER,
        ModuleCategory::MISC
    };

    static ImU32 CategoryColor(ModuleCategory c) {
        switch (c) {
            case ModuleCategory::COMBAT:   return iOS::Col::Red;
            case ModuleCategory::MOVEMENT: return iOS::Col::Blue;
            case ModuleCategory::VISUAL:   return iOS::Col::Purple;
            case ModuleCategory::PLAYER:   return iOS::Col::Green;
            default:                       return iOS::Col::Orange;
        }
    }

    static void KeyLabel(int key, char* out, size_t n) {
        if (key >= 'A' && key <= 'Z')  snprintf(out, n, "%c", (char)key);
        else if (key == VK_CONTROL)    snprintf(out, n, "CTRL");
        else if (key == VK_SHIFT)      snprintf(out, n, "SHIFT");
        else if (key == VK_MENU)       snprintf(out, n, "ALT");
        else if (key == VK_INSERT)     snprintf(out, n, "INS");
        else if (key > 0)              snprintf(out, n, "0x%X", key);
        else                           out[0] = '\0';
    }

    // Visible switch state: the pending value if one is in flight,
    // otherwise the module's real state.
    static bool VisualState(Module* mod) {
        auto it = s_pending.find(mod->GetName());
        if (it == s_pending.end()) return mod->IsEnabled();

        if (mod->IsEnabled() == it->second) {   // the tick caught up
            s_pending.erase(it);
            return mod->IsEnabled();
        }
        return it->second;
    }

    // ---------------------------------------------------------
    // One module: switch, keybind chip, expandable settings
    // ---------------------------------------------------------
    static void ModuleRow(const std::shared_ptr<Module>& mod, bool last) {
        ImGui::PushID(mod.get());

        const std::string& name = mod->GetName();
        bool on = VisualState(mod.get());
        bool& expanded = s_expanded[name];

        float w = ImGui::GetContentRegionAvail().x;
        float h = iOS::M::RowHeight;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImU32 accent = CategoryColor(mod->GetCategory());

        // Everything left of the switch expands the row
        float switchZone = iOS::M::SwitchW + iOS::M::RowPadX * 2.0f;
        ImGui::InvisibleButton("##expand", ImVec2(w - switchZone, h));
        bool expandClicked = ImGui::IsItemClicked();
        bool expandHeld    = ImGui::IsItemActive();
        if (expandClicked) expanded = !expanded;

        float pressT = iOS::Anim::To(ImGui::GetID("##rp"),
                                     expandHeld ? 1.0f : 0.0f, 22.0f);
        if (pressT > 0.01f) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                              iOS::Col::Alpha(iOS::Col::CardPressed, pressT));
        }

        // Category dot brightens with the module
        float onT = iOS::Anim::To(ImGui::GetID("##dot"), on ? 1.0f : 0.0f, 14.0f);
        float dotX = p.x + iOS::M::RowPadX + 4.0f;
        float cy = p.y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(dotX, cy), 4.0f,
                            iOS::Col::Alpha(accent, 0.28f + 0.72f * onT), 14);

        float textX = dotX + 14.0f;
        ImVec2 ts = ImGui::CalcTextSize(name.c_str());
        dl->AddText(ImVec2(textX, cy - ts.y * 0.5f), iOS::Col::Label, name.c_str());

        // Chevron rotates as the row opens
        float openT = iOS::Anim::To(ImGui::GetID("##ch"), expanded ? 1.0f : 0.0f, 16.0f);
        {
            float cx = p.x + w - switchZone - 6.0f;
            float ang = openT * 1.5707963f;
            float ca = std::cos(ang), sa = std::sin(ang);
            auto rot = [&](float x, float y) {
                return ImVec2(cx + (x * ca - y * sa), cy + (x * sa + y * ca));
            };
            dl->AddLine(rot(-2.5f, -4.5f), rot(2.0f, 0.0f), iOS::Col::Label3, 1.8f);
            dl->AddLine(rot(2.0f, 0.0f), rot(-2.5f, 4.5f), iOS::Col::Label3, 1.8f);
        }

        // Keybind chip
        int key = mod->GetKeybind();
        if (key > 0) {
            char kb[16];
            KeyLabel(key, kb, sizeof(kb));
            if (kb[0]) {
                iOS::Fonts::Push(iOS::Fonts::Caption);
                ImVec2 bs = ImGui::CalcTextSize(kb);
                float bw = bs.x + 12.0f;
                float bx = p.x + w - switchZone - 24.0f - bw;
                float by = cy - 10.0f;
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + 20.0f),
                                  iOS::Col::Fill, 6.0f);
                dl->AddText(ImVec2(bx + 6.0f, by + (20.0f - bs.y) * 0.5f),
                            iOS::Col::Label2, kb);
                iOS::Fonts::Pop(iOS::Fonts::Caption);
            }
        }

        // Switch
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - iOS::M::SwitchW - iOS::M::RowPadX,
                                         cy - iOS::M::SwitchH * 0.5f));
        bool before = on;
        if (iOS::Switch("sw", &on) && on != before) {
            s_pending[name] = on;
            ModuleManager::QueueToggle(mod.get());
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));

        // ---- Settings, animated open ----
        if (iOS::BeginCollapsible("body", expanded)) {
            ImGui::Indent(iOS::M::RowPadX);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x
                                 - iOS::M::RowPadX - 60.0f);

            const std::string& desc = mod->GetDescription();
            if (!desc.empty()) {
                iOS::Fonts::Push(iOS::Fonts::Caption);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(iOS::Col::Label2));
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 8.0f);
                ImGui::TextUnformatted(desc.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                iOS::Fonts::Pop(iOS::Fonts::Caption);
                ImGui::Dummy(ImVec2(0, 4));
            }

            // Modules still draw stock ImGui controls. The base style
            // was pulled toward the same language so they do not look
            // pasted in from another program.
            mod->RenderSettings();

            ImGui::PopItemWidth();
            ImGui::Unindent(iOS::M::RowPadX);
            ImGui::Dummy(ImVec2(0, 6));
        }
        iOS::EndCollapsible();

        if (!last) iOS::RowSeparator(iOS::M::RowPadX + 18.0f);

        ImGui::PopID();
    }

    static void RenderCategory(ModuleCategory cat) {
        auto mods = ModuleManager::GetModulesByCategory(cat);

        if (mods.empty()) {
            ImGui::Dummy(ImVec2(0, 40));
            const char* msg = "Nothing here yet";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(p.x + (w - ts.x) * 0.5f, p.y), iOS::Col::Label3, msg);
            ImGui::Dummy(ImVec2(0, 30));
            return;
        }

        int enabled = 0;
        for (auto& m : mods) if (m->IsEnabled()) enabled++;

        char head[64];
        snprintf(head, sizeof(head), "%d of %d active",
                 enabled, (int)mods.size());
        iOS::SectionHeader(head);

        iOS::BeginCard();
        for (size_t i = 0; i < mods.size(); i++)
            ModuleRow(mods[i], i + 1 == mods.size());
        iOS::EndCard();

        ImGui::Dummy(ImVec2(0, 6));
    }

    static void RenderConfigs() {
        iOS::SectionHeader("Profiles");

        auto profiles = Profiles::All();
        for (size_t i = 0; i < profiles.size(); i++) {
            ImGui::PushID((int)i);

            iOS::BeginCard();
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Indent(iOS::M::RowPadX);

            iOS::Fonts::Push(iOS::Fonts::BodyBold);
            ImGui::TextUnformatted(profiles[i].name);
            iOS::Fonts::Pop(iOS::Fonts::BodyBold);

            iOS::Fonts::Push(iOS::Fonts::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(iOS::Col::Label2));
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 8.0f);
            ImGui::TextUnformatted(profiles[i].description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            iOS::Fonts::Pop(iOS::Fonts::Caption);

            ImGui::Dummy(ImVec2(0, 8));

            if (iOS::Button("Load", ImGui::GetContentRegionAvail().x
                                    - iOS::M::RowPadX)) {
                Profile p = profiles[i];
                // Every switch is about to move, so drop the pending
                // map rather than fight it.
                s_pending.clear();
                ModuleManager::QueueAction([p](JNIEnv* env) {
                    Profiles::Apply(p, env);
                });
            }

            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Unindent(iOS::M::RowPadX);
            iOS::EndCard();

            ImGui::PopID();
        }

        const std::string& report = Profiles::LastReport();
        if (!report.empty()) iOS::Footnote(report.c_str());

        ImGui::Dummy(ImVec2(0, 8));
        iOS::HUD::RenderSettings();

        iOS::SectionHeader("About");
        iOS::BeginCard();
        iOS::ValueRow("Version", "2.4.1");
        iOS::RowSeparator();
        iOS::ValueRow("Target", "Lunar 1.8.9");
        iOS::RowSeparator();
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", ModuleManager::GetModuleCount());
            iOS::ValueRow("Modules", buf);
        }
        iOS::EndCard();

        iOS::Footnote("INSERT opens this menu. DELETE ejects the client.");
        ImGui::Dummy(ImVec2(0, 10));
    }

    // ---------------------------------------------------------
    // Header: large title, then the segmented control
    // ---------------------------------------------------------
    static void RenderHeader(float width) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float h = 96.0f;

        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h),
                          iOS::Col::Card,
                          20.0f, ImDrawFlags_RoundCornersTop);

        // App mark
        float ix = p.x + 20.0f, iy = p.y + 18.0f, is = 30.0f;
        dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + is, iy + is),
                          iOS::Col::Blue, 8.0f);
        iOS::Fonts::Push(iOS::Fonts::BodyBold);
        ImVec2 ps = ImGui::CalcTextSize("P");
        dl->AddText(ImVec2(ix + (is - ps.x) * 0.5f, iy + (is - ps.y) * 0.5f),
                    iOS::Col::OnAccent, "P");
        iOS::Fonts::Pop(iOS::Fonts::BodyBold);

        iOS::Fonts::Push(iOS::Fonts::Title);
        dl->AddText(ImVec2(ix + is + 12.0f, iy - 1.0f), iOS::Col::Label, "Phantom");
        iOS::Fonts::Pop(iOS::Fonts::Title);

        // Live module count on the right
        int active = 0;
        for (auto& m : ModuleManager::GetModules()) if (m->IsEnabled()) active++;

        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d active", active);
        iOS::Fonts::Push(iOS::Fonts::Caption);
        ImVec2 cs = ImGui::CalcTextSize(cnt);
        float bw = cs.x + 18.0f;
        float bx = p.x + width - 20.0f - bw;
        dl->AddRectFilled(ImVec2(bx, iy + 5.0f), ImVec2(bx + bw, iy + 25.0f),
                          active ? iOS::Col::BlueSoft : iOS::Col::Fill, 10.0f);
        dl->AddText(ImVec2(bx + 9.0f, iy + 5.0f + (20.0f - cs.y) * 0.5f),
                    active ? iOS::Col::Blue : iOS::Col::Label2, cnt);
        iOS::Fonts::Pop(iOS::Fonts::Caption);

        // Segmented control
        ImGui::SetCursorScreenPos(ImVec2(p.x + 16.0f, p.y + 56.0f));
        iOS::Segmented("tabs", s_tabNames, kTabCount, &s_tab, width - 32.0f);

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));

        dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + width, p.y + h - 0.5f),
                    iOS::Col::Separator, 1.0f);
    }

public:
    static void ApplyTheme() {
        iOS::Fonts::Load();
        iOS::ApplyStyle();
    }

    // Called every frame. Handles its own fade so closing the menu
    // is a dissolve rather than a cut.
    static void Render(bool open) {
        s_open = open;
        s_fade = iOS::Anim::ToStr("menuFade", open ? 1.0f : 0.0f, 16.0f);

        if (s_fade < 0.004f) return;

        const float W = 470.0f;
        const float H = 560.0f;

        ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(120, 90), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(760, 1000));

        // Scale up slightly as it appears, the standard iOS present
        float scale = 0.97f + 0.03f * s_fade;
        ImGui::SetNextWindowBgAlpha(s_fade);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_fade);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;

        if (!open) flags |= ImGuiWindowFlags_NoInputs;   // no clicks mid-fade

        if (ImGui::Begin("##phantom", nullptr, flags)) {
            float width = ImGui::GetWindowWidth();

            // Grey grouped background behind the whole sheet
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRectFilled(
                wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                iOS::Col::Alpha(iOS::Col::GroupedBg, s_fade), 20.0f);

            RenderHeader(width);

            ImGui::BeginChild("##scroll", ImVec2(0, 0),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            ImGui::Indent(16.0f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 32.0f);

            if (s_tab == 5) RenderConfigs();
            else            RenderCategory(s_tabCats[s_tab]);

            ImGui::PopItemWidth();
            ImGui::Unindent(16.0f);
            ImGui::Dummy(ImVec2(0, 12));
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::PopStyleVar();
        (void)scale;

        iOS::Anim::GarbageCollect();
    }

    static void RenderHUD() {
        iOS::HUD::Render();
    }

    // World overlays, every frame regardless of menu state.
    // Backtrack draws after the ESP so the held position sits on top
    // of the box marking where the server actually has the player.
    static void RenderOverlays() {
        auto esp = ModuleManager::GetESP();
        if (esp) esp->RenderESP();

        auto bt = ModuleManager::GetBacktrack();
        if (bt) BacktrackVis::Render(bt.get());
    }
};
