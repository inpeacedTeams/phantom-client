#pragma once
#include <imgui.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <memory>
#include <unordered_map>

#include "ios_theme.h"
#include "ios_widgets.h"
#include "ios_hud.h"
#include "../mc/mouse_control.h"
#include "../input/key_capture.h"
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
// MOTION
// Everything that moves is a function of elapsed time, never of
// frame count, so it looks the same at 60 and at 400 FPS:
//
//   the sheet    fades and lifts into place on open
//   the rows     arrive staggered when a tab changes, which is what
//                makes switching feel like a screen transition
//                rather than a redraw
//   the switches ease, and the row's accent dot gains a halo
//   the chips    pulse while they are waiting for a key
//
// Nothing bounces or spins. The point is to make state changes
// legible, not decorative.
//
// KEYBIND PICKER
// The chip on each row is a button. Click it and the client listens
// for the next key through KeyCapture, which reads real window
// messages rather than scanning 256 virtual keys and guessing.
// While a capture is live, module hotkeys are suppressed, so
// binding R does not also toggle whatever R was bound to.
//
//   ESC        cancel, keep the old bind
//   BACKSPACE  clear the bind
//
// TWO LEVELS OF SETTINGS
// A module's panel shows its mode and the two or three values worth
// touching. Everything else is behind Advanced. The full set is
// still bound, so profiles reach all of it.
//
// OPTIMISTIC SWITCHES
// Because a toggle is queued, IsEnabled() still reads the old value
// for up to one tick. Animating from that would make every switch
// stutter, so the pending value is held locally and shown at once,
// then dropped when the real state agrees.
// =================================================================

class Menu {
private:
    inline static int   s_tab  = 0;
    inline static float s_fade = 0.0f;

    // Tab transition
    inline static int   s_lastTab = -1;
    inline static float s_tabAge  = 0.0f;

    inline static std::unordered_map<std::string, bool> s_pending;
    inline static std::unordered_map<std::string, bool> s_expanded;
    inline static std::unordered_map<std::string, bool> s_advanced;

    // Which module is currently waiting for a key. Empty means none.
    inline static std::string s_binding;
    inline static float s_bindClock = 0.0f;

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

    static float Clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // Decelerating ease. Fast at the start, settles softly, which is
    // the curve iOS uses for anything entering the screen.
    static float EaseOut(float t) {
        float u = 1.0f - Clamp01(t);
        return 1.0f - u * u * u;
    }

    // How far along is row `index` in the tab transition?
    static float RowEntry(int index) {
        const float step = 0.035f;   // gap between neighbouring rows
        const float dur  = 0.24f;
        return EaseOut((s_tabAge - step * (float)index) / dur);
    }

    // The pending value if a toggle is in flight, else the real one
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
    // Keybind chip
    // ---------------------------------------------------------
    // Draws the chip and handles the click. Returns the width it
    // used, so the caller knows where the rest of the row ends.
    //
    // Submitted AFTER the row's expand button so it wins the hover
    // test: in ImGui the later item claims the hovered id, which is
    // exactly the layering we want here.
    // ---------------------------------------------------------
    static float KeybindChip(const std::shared_ptr<Module>& mod,
                             ImVec2 rightEdge, float cy, float entry)
    {
        const std::string& name = mod->GetName();
        bool capturing = (s_binding == name);

        char label[40];
        if (capturing) {
            snprintf(label, sizeof(label), "Press a key...");
        } else {
            int key = mod->GetKeybind();
            if (key > 0) KeyCapture::Label(key, label, sizeof(label));
            else         snprintf(label, sizeof(label), "+");
        }

        iOS::Fonts::Push(iOS::Fonts::Caption);
        ImVec2 ts = ImGui::CalcTextSize(label);
        iOS::Fonts::Pop(iOS::Fonts::Caption);

        float bw = ts.x + 16.0f;
        if (bw < 30.0f) bw = 30.0f;
        float bh = 21.0f;

        ImVec2 a(rightEdge.x - bw, cy - bh * 0.5f);
        ImVec2 b(rightEdge.x, cy + bh * 0.5f);

        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##bind", ImVec2(bw, bh));

        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();

        if (clicked) {
            if (capturing) {
                KeyCapture::Cancel();
                s_binding.clear();
            } else {
                KeyCapture::Begin();
                s_binding = name;
                s_bindClock = 0.0f;
            }
        }

        float hov = iOS::Anim::To(ImGui::GetID("##bindh"),
                                  hovered ? 1.0f : 0.0f, 18.0f);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImU32 bg, fg;
        if (capturing) {
            // Breathing highlight. A static "waiting" state reads as
            // a stuck UI; a slow pulse reads as listening.
            float pulse = 0.5f + 0.5f * std::sin(s_bindClock * 6.0f);
            bg = iOS::Col::Alpha(iOS::Col::Blue, (0.16f + 0.16f * pulse) * entry);
            fg = iOS::Col::Alpha(iOS::Col::Blue, entry);
            dl->AddRect(a, b, iOS::Col::Alpha(iOS::Col::Blue,
                        (0.35f + 0.35f * pulse) * entry), 6.0f, 0, 1.2f);
        } else if (mod->GetKeybind() > 0) {
            bg = iOS::Col::Alpha(
                iOS::Col::Mix(iOS::Col::Fill, iOS::Col::BlueSoft, hov), entry);
            fg = iOS::Col::Alpha(
                iOS::Col::Mix(iOS::Col::Label2, iOS::Col::Blue, hov), entry);
        } else {
            // Unbound: nearly invisible until you go looking for it
            bg = iOS::Col::Alpha(iOS::Col::Fill, (0.35f + 0.65f * hov) * entry);
            fg = iOS::Col::Alpha(
                iOS::Col::Mix(iOS::Col::Label3, iOS::Col::Blue, hov), entry);
        }

        dl->AddRectFilled(a, b, bg, 6.0f);

        iOS::Fonts::Push(iOS::Fonts::Caption);
        dl->AddText(ImVec2(a.x + (bw - ts.x) * 0.5f, cy - ts.y * 0.5f), fg, label);
        iOS::Fonts::Pop(iOS::Fonts::Caption);

        if (hovered && !capturing) {
            ImGui::SetTooltip(mod->GetKeybind() > 0
                ? "Click to rebind. Backspace clears, Escape cancels."
                : "Click to set a key.");
        }

        return bw;
    }

    // Consume whatever the capture produced. Runs once per frame,
    // outside the row loop, because the module list may be rebuilt
    // between frames and the result has to survive that.
    static void PollBinding(float dt) {
        if (s_binding.empty()) return;
        s_bindClock += dt;

        int key = 0;
        KeyCapture::Result r = KeyCapture::Poll(&key);
        if (r == KeyCapture::None) {
            // The capture can also be cancelled from the window
            // procedure, for instance when focus is lost.
            if (!KeyCapture::IsActive()) s_binding.clear();
            return;
        }

        std::string target = s_binding;
        s_binding.clear();

        if (r == KeyCapture::Cancelled) return;

        int newKey = (r == KeyCapture::Cleared) ? 0 : key;

        // The bind is read on the client thread every tick, so it is
        // set there rather than written from under it.
        ModuleManager::QueueAction([target, newKey](JNIEnv*) {
            for (auto& m : ModuleManager::GetModules()) {
                if (m->GetName() != target) continue;
                // Two modules on one key means one toggle fires both.
                if (newKey > 0) {
                    for (auto& other : ModuleManager::GetModules())
                        if (other != m && other->GetKeybind() == newKey)
                            other->SetKeybind(0);
                }
                m->SetKeybind(newKey);
                break;
            }
        });
    }

    // ---------------------------------------------------------
    // Advanced disclosure inside an open module
    // ---------------------------------------------------------
    static void AdvancedBlock(const std::shared_ptr<Module>& mod) {
        if (!mod->HasAdvanced()) return;

        bool& open = s_advanced[mod->GetName()];

        ImGui::Dummy(ImVec2(0, 2));

        float w = ImGui::GetContentRegionAvail().x;
        float h = 30.0f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton("##adv", ImVec2(w, h));
        if (ImGui::IsItemClicked()) open = !open;

        float hov = iOS::Anim::To(ImGui::GetID("##advh"),
                                  ImGui::IsItemHovered() ? 1.0f : 0.0f, 20.0f);
        float rot = iOS::Anim::To(ImGui::GetID("##advr"), open ? 1.0f : 0.0f, 16.0f);

        float cy = p.y + h * 0.5f;

        // Chevron, pointing right when closed and down when open
        {
            float cx = p.x + 5.0f;
            float ang = rot * 1.5707963f;
            float ca = std::cos(ang), sa = std::sin(ang);
            auto R = [&](float x, float y) {
                return ImVec2(cx + (x * ca - y * sa), cy + (x * sa + y * ca));
            };
            ImU32 c = iOS::Col::Mix(iOS::Col::Label3, iOS::Col::Blue, hov);
            dl->AddLine(R(-2.0f, -4.0f), R(2.0f, 0.0f), c, 1.7f);
            dl->AddLine(R(2.0f, 0.0f), R(-2.0f, 4.0f), c, 1.7f);
        }

        iOS::Fonts::Push(iOS::Fonts::Caption);
        ImVec2 ts = ImGui::CalcTextSize("Advanced");
        dl->AddText(ImVec2(p.x + 16.0f, cy - ts.y * 0.5f),
                    iOS::Col::Mix(iOS::Col::Label2, iOS::Col::Blue, hov),
                    "Advanced");
        iOS::Fonts::Pop(iOS::Fonts::Caption);

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));

        if (iOS::BeginCollapsible("advbody", open)) {
            mod->RenderAdvanced();
            ImGui::Dummy(ImVec2(0, 4));
            iOS::EndCollapsible();
        }
    }

    // ---------------------------------------------------------
    // One module: switch, keybind chip, expandable settings
    // ---------------------------------------------------------
    static void ModuleRow(const std::shared_ptr<Module>& mod, bool last,
                          float entry)
    {
        ImGui::PushID(mod.get());

        const std::string& name = mod->GetName();
        bool on = VisualState(mod.get());
        bool& expanded = s_expanded[name];

        // Rows slide in from the right as a tab opens
        float slide = (1.0f - entry) * 16.0f;

        float w = ImGui::GetContentRegionAvail().x;
        float h = iOS::M::RowHeight;
        ImVec2 p = ImGui::GetCursorScreenPos();
        p.x += slide;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImU32 accent = CategoryColor(mod->GetCategory());
        float cy = p.y + h * 0.5f;

        // Everything left of the switch expands the row. Submitted
        // first so the chip, submitted later, sits on top of it.
        float switchZone = iOS::M::SwitchW + iOS::M::RowPadX * 2.0f;
        ImGui::InvisibleButton("##expand", ImVec2(w - switchZone, h));
        bool expandClicked = ImGui::IsItemClicked();
        bool expandHeld    = ImGui::IsItemActive();
        bool expandHover   = ImGui::IsItemHovered();

        // Hover is a whisper, press is a proper flash
        float hovT = iOS::Anim::To(ImGui::GetID("##rh"),
                                   expandHover ? 1.0f : 0.0f, 18.0f);
        float pressT = iOS::Anim::To(ImGui::GetID("##rp"),
                                     expandHeld ? 1.0f : 0.0f, 22.0f);
        float wash = pressT > hovT * 0.35f ? pressT : hovT * 0.35f;
        if (wash > 0.01f) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                              iOS::Col::Alpha(iOS::Col::CardPressed, wash * entry));
        }

        // Category dot brightens with the module
        float onT = iOS::Anim::To(ImGui::GetID("##dot"), on ? 1.0f : 0.0f, 14.0f);
        float dotX = p.x + iOS::M::RowPadX + 4.0f;

        // A soft halo while it is on, so an active module reads from
        // across the panel without shouting
        if (onT > 0.02f) {
            dl->AddCircleFilled(ImVec2(dotX, cy), 4.0f + 4.0f * onT,
                                iOS::Col::Alpha(accent, 0.18f * onT * entry), 16);
        }
        dl->AddCircleFilled(ImVec2(dotX, cy), 4.0f,
            iOS::Col::Alpha(accent, (0.28f + 0.72f * onT) * entry), 14);

        float textX = dotX + 14.0f;

        // A live status line, if the module has one, sits under the
        // name so you can read what it is doing without opening it.
        const char* status = on ? mod->StatusLine() : nullptr;

        if (status && status[0]) {
            ImVec2 ns = ImGui::CalcTextSize(name.c_str());
            dl->AddText(ImVec2(textX, cy - ns.y + 1.0f),
                        iOS::Col::Alpha(iOS::Col::Label, entry), name.c_str());

            iOS::Fonts::Push(iOS::Fonts::Caption);
            dl->AddText(ImVec2(textX, cy + 2.0f),
                        iOS::Col::Alpha(accent, 0.85f * entry), status);
            iOS::Fonts::Pop(iOS::Fonts::Caption);
        } else {
            ImVec2 ts = ImGui::CalcTextSize(name.c_str());
            dl->AddText(ImVec2(textX, cy - ts.y * 0.5f),
                        iOS::Col::Alpha(iOS::Col::Label, entry), name.c_str());
        }

        // ---- Keybind chip ----
        float chipRight = p.x + w - switchZone - 22.0f;
        float chipW = KeybindChip(mod, ImVec2(chipRight, cy), cy, entry);

        // Chevron rotates as the row opens
        float openT = iOS::Anim::To(ImGui::GetID("##ch"), expanded ? 1.0f : 0.0f, 16.0f);
        {
            float cx = p.x + w - switchZone - 6.0f;
            float ang = openT * 1.5707963f;
            float ca = std::cos(ang), sa = std::sin(ang);
            auto rot = [&](float x, float y) {
                return ImVec2(cx + (x * ca - y * sa), cy + (x * sa + y * ca));
            };
            ImU32 c = iOS::Col::Alpha(
                iOS::Col::Mix(iOS::Col::Label3, iOS::Col::Label2, hovT), entry);
            dl->AddLine(rot(-2.5f, -4.5f), rot(2.0f, 0.0f), c, 1.8f);
            dl->AddLine(rot(2.0f, 0.0f), rot(-2.5f, 4.5f), c, 1.8f);
        }
        (void)chipW;

        // The row only expands if the click was not on the chip.
        // ImGui gives the later item the hover, so this is just a
        // matter of asking after both have been submitted.
        if (expandClicked && s_binding != name) expanded = !expanded;

        // ---- Switch ----
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - iOS::M::SwitchW - iOS::M::RowPadX,
                                         cy - iOS::M::SwitchH * 0.5f));
        bool before = on;
        if (iOS::Switch("sw", &on) && on != before) {
            s_pending[name] = on;
            ModuleManager::QueueToggle(mod.get());
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x - slide, p.y + h));

        // ---- Settings, animated open ----
        // EndCollapsible only balances a Begin that returned true.
        if (iOS::BeginCollapsible("body", expanded)) {
            ImGui::Indent(iOS::M::RowPadX);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x
                                 - iOS::M::RowPadX - 56.0f);

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

            // The everyday controls. Modules still draw stock ImGui
            // widgets; the base style was pulled toward the same
            // language so they do not look pasted in.
            mod->RenderSettings();

            // Everything else, folded away
            AdvancedBlock(mod);

            ImGui::PopItemWidth();
            ImGui::Unindent(iOS::M::RowPadX);
            ImGui::Dummy(ImVec2(0, 6));

            iOS::EndCollapsible();
        }

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
        snprintf(head, sizeof(head), "%d of %d active", enabled, (int)mods.size());
        iOS::SectionHeader(head);

        iOS::BeginCard();
        for (size_t i = 0; i < mods.size(); i++)
            ModuleRow(mods[i], i + 1 == mods.size(), RowEntry((int)i));
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

            if (iOS::Button("Load",
                    ImGui::GetContentRegionAvail().x - iOS::M::RowPadX)) {
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
        iOS::ValueRow("Version", "2.6.0");
        iOS::RowSeparator();
        iOS::ValueRow("Target", "Lunar 1.8.9");
        iOS::RowSeparator();
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", ModuleManager::GetModuleCount());
            iOS::ValueRow("Modules", buf);
        }
        iOS::RowSeparator();
        iOS::ValueRow("Cursor",
            MouseControl::IsUsable() ? "Released by the game" : "Software fallback");
        iOS::EndCard();

        iOS::Footnote("Click any key chip to rebind it. INSERT opens this menu, "
                      "ESC closes it, DELETE ejects.");
        ImGui::Dummy(ImVec2(0, 10));
    }

    // ---------------------------------------------------------
    // Header: app mark, large title, live count, tabs
    // ---------------------------------------------------------
    static void RenderHeader(float width) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float h = 96.0f;

        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h),
                          iOS::Col::Card, 20.0f, ImDrawFlags_RoundCornersTop);

        float ix = p.x + 20.0f, iy = p.y + 18.0f, is = 30.0f;
        dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + is, iy + is),
                          iOS::Col::Blue, 8.0f);
        iOS::Fonts::Push(iOS::Fonts::BodyBold);
        ImVec2 ps = ImGui::CalcTextSize("P");
        dl->AddText(ImVec2(ix + (is - ps.x) * 0.5f, iy + (is - ps.y) * 0.5f),
                    iOS::Col::OnAccent, "P");
        iOS::Fonts::Pop(iOS::Fonts::BodyBold);

        iOS::Fonts::Push(iOS::Fonts::Title);
        dl->AddText(ImVec2(ix + is + 12.0f, iy - 2.0f), iOS::Col::Label, "Phantom");
        iOS::Fonts::Pop(iOS::Fonts::Title);

        int active = 0;
        for (auto& m : ModuleManager::GetModules()) if (m->IsEnabled()) active++;

        // The count pill eases between values rather than jumping,
        // so toggling something reads as one continuous change.
        float shownActive = iOS::Anim::ToStr("activeCount", (float)active, 12.0f);

        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d active", (int)(shownActive + 0.5f));
        iOS::Fonts::Push(iOS::Fonts::Caption);
        ImVec2 cs = ImGui::CalcTextSize(cnt);
        float bw = cs.x + 18.0f;
        float bx = p.x + width - 20.0f - bw;
        float lit = Clamp01(shownActive);
        dl->AddRectFilled(ImVec2(bx, iy + 5.0f), ImVec2(bx + bw, iy + 25.0f),
                          iOS::Col::Mix(iOS::Col::Fill, iOS::Col::BlueSoft, lit), 10.0f);
        dl->AddText(ImVec2(bx + 9.0f, iy + 5.0f + (20.0f - cs.y) * 0.5f),
                    iOS::Col::Mix(iOS::Col::Label2, iOS::Col::Blue, lit), cnt);
        iOS::Fonts::Pop(iOS::Fonts::Caption);

        ImGui::SetCursorScreenPos(ImVec2(p.x + 16.0f, p.y + 56.0f));
        iOS::Segmented("tabs", s_tabNames, kTabCount, &s_tab, width - 32.0f);

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        dl->AddLine(ImVec2(p.x, p.y + h - 0.5f),
                    ImVec2(p.x + width, p.y + h - 0.5f),
                    iOS::Col::Separator, 1.0f);
    }

public:
    static void ApplyTheme() {
        iOS::Fonts::Load();
        iOS::ApplyStyle();
    }

    // Called every frame. Owns its fade, so closing is a dissolve
    // rather than a cut.
    static void Render(bool open) {
        float dt = ImGui::GetIO().DeltaTime;
        if (dt > 0.1f) dt = 0.1f;

        // A capture must resolve even if the menu is closing, or the
        // client is left listening for a key nobody is going to press.
        PollBinding(dt);

        s_fade = iOS::Anim::ToStr("menuFade", open ? 1.0f : 0.0f, 16.0f);

        if (!open && !s_binding.empty()) {
            KeyCapture::Cancel();
            s_binding.clear();
        }

        if (s_fade < 0.004f) {
            s_tabAge = 0.0f;      // next open replays the entry
            s_lastTab = -1;
            return;
        }

        // Tab transition clock. Reset on a change, then run forward.
        if (s_tab != s_lastTab) {
            s_lastTab = s_tab;
            s_tabAge = 0.0f;
        } else if (s_tabAge < 3.0f) {
            s_tabAge += dt;
        }

        ImGui::SetNextWindowSize(ImVec2(470, 560), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(120, 90), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(760, 1000));
        ImGui::SetNextWindowBgAlpha(0.0f);   // the sheet draws its own

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_fade);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground;

        // Mid-fade the menu is on its way out: visible but not
        // clickable, or a stray click lands on a ghost.
        if (!open) flags |= ImGuiWindowFlags_NoInputs;

        if (ImGui::Begin("##phantom", nullptr, flags)) {
            float width = ImGui::GetWindowWidth();

            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Sheet: shadow, then the grouped grey ground. The shadow
            // grows with the fade, so the panel reads as lifting off
            // the game rather than being switched on.
            float lift = s_fade;
            dl->AddRectFilled(ImVec2(wp.x + 2.0f, wp.y + 4.0f + 4.0f * lift),
                              ImVec2(wp.x + ws.x + 2.0f, wp.y + ws.y + 4.0f + 6.0f * lift),
                              IM_COL32(0, 0, 0, (int)(52 * s_fade)), 22.0f);
            dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                              iOS::Col::GroupedBg, 20.0f);

            RenderHeader(width);

            ImGui::BeginChild("##scroll", ImVec2(0, 0),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);

            // Content lifts into place behind the header
            ImGui::Dummy(ImVec2(0, (1.0f - EaseOut(s_fade)) * 14.0f));

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
