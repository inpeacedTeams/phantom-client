#pragma once
#include <imgui.h>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "ios_theme.h"
#include "ios_widgets.h"
#include "ios_hud.h"
#include "config_panel.h"
#include "notifications.h"
#include "../mc/mouse_control.h"
#include "../input/key_capture.h"
#include "../modules/module_manager.h"
#include "../render/backtrack_vis.h"

// =================================================================
// Menu
// =================================================================
// An iOS Settings screen: grouped white cards on grey, one switch
// per row, a segmented control instead of tabs.
//
// Runs entirely on the render thread and must never call JNI.
// Toggles and config loads are queued to ModuleManager and applied
// on the client thread where a valid JNIEnv exists.
//
// WHAT THIS FILE OWNS
// The shell, the module list and the key picker. Configs live in
// ConfigPanel and the HUD owns its own settings, because a class
// that draws everything is a class nobody wants to open.
//
// MOTION
// Everything that moves is a function of elapsed time, never of
// frame count, so it looks the same at 60 and at 400 FPS. Nothing
// bounces or spins: the point is to make state changes legible,
// not decorative.
//
// SEARCH
// Typing anything collapses the tabs and shows every match from
// every category, because when you are looking for a module you do
// not want to first remember which tab it lives in. Matching is on
// the name AND the description, so "knockback" finds Velocity.
//
// KEYBIND PICKER
// Clicking a key chip opens a full-screen overlay and listens for
// the next key through KeyCapture, which reads real window messages
// rather than scanning 256 virtual keys and guessing. Module
// hotkeys are suppressed while it is open, so binding R does not
// also toggle whatever R was bound to.
//
//   any key    binds it
//   BACKSPACE  sets None
//   ESC        cancels, the old bind survives
//
// SCALE
// UI::Apply writes the user's scale into FontGlobalScale, which is
// baked into vertices at AddText time. It is therefore set at the
// top of Render and put back at the end, so the HUD, the ESP
// nametags and the intro keep their own size.
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

    // Which module is waiting for a key. Empty means none.
    inline static std::string s_binding;
    inline static float s_bindClock = 0.0f;
    inline static float s_bindFade  = 0.0f;
    inline static std::string s_bindResult;
    inline static float s_bindResultFade = 0.0f;

    // Search
    inline static char s_search[64] = {};

    // Hover description
    inline static std::string s_hoverName;
    inline static std::string s_hoverDesc;
    inline static float s_hoverFade = 0.0f;
    inline static ImVec2 s_hoverAnchor{ 0, 0 };

    // Player and Misc used to be separate tabs and both were
    // permanently empty. Two dead tabs out of seven is not a
    // category system, it is a promise the client does not keep, so
    // they share one and the space went to the things that exist.
    enum Tab { TAB_COMBAT = 0, TAB_MOVE, TAB_VISUAL, TAB_MISC,
               TAB_CONFIG, TAB_UI, TAB_COUNT };

    inline static const char* s_tabNames[TAB_COUNT] = {
        "Combat", "Move", "Visual", "Misc", "Configs", "UI"
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

    static const char* CategoryName(ModuleCategory c) {
        switch (c) {
            case ModuleCategory::COMBAT:   return "Combat";
            case ModuleCategory::MOVEMENT: return "Movement";
            case ModuleCategory::VISUAL:   return "Visual";
            case ModuleCategory::PLAYER:   return "Player";
            default:                       return "Misc";
        }
    }

    static bool InTab(ModuleCategory c, int tab) {
        switch (tab) {
            case TAB_COMBAT: return c == ModuleCategory::COMBAT;
            case TAB_MOVE:   return c == ModuleCategory::MOVEMENT;
            case TAB_VISUAL: return c == ModuleCategory::VISUAL;
            case TAB_MISC:   return c == ModuleCategory::PLAYER
                                 || c == ModuleCategory::MISC;
            default:         return false;
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
        if (!iOS::UI::openAnimation) return 1.0f;
        const float step = 0.035f;   // gap between neighbouring rows
        const float dur  = 0.24f;
        return EaseOut((s_tabAge - step * (float)index) / dur);
    }

    // Case-insensitive substring. Small enough that a real search
    // library would be a joke, and the module list is fifteen items.
    static bool Contains(const std::string& hay, const char* needle) {
        if (!needle || !needle[0]) return true;
        size_t nl = std::strlen(needle);
        if (nl > hay.size()) return false;
        for (size_t i = 0; i + nl <= hay.size(); i++) {
            size_t j = 0;
            while (j < nl &&
                   std::tolower((unsigned char)hay[i + j]) ==
                   std::tolower((unsigned char)needle[j])) j++;
            if (j == nl) return true;
        }
        return false;
    }

    static bool Matches(const std::shared_ptr<Module>& m, const char* q) {
        if (!q || !q[0]) return true;
        return Contains(m->GetName(), q)
            || Contains(m->GetDescription(), q)
            || Contains(CategoryName(m->GetCategory()), q);
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
    // Submitted AFTER the row's expand button so it wins the hover
    // test: in ImGui the later item claims the hovered id, which is
    // exactly the layering wanted here.
    // ---------------------------------------------------------
    static void KeybindChip(const std::shared_ptr<Module>& mod,
                            float rightX, float cy, float entry)
    {
        const std::string& name = mod->GetName();
        bool capturing = (s_binding == name);

        char label[40];
        int key = mod->GetKeybind();
        if (capturing)    snprintf(label, sizeof(label), "...");
        else if (key > 0) KeyCapture::Label(key, label, sizeof(label));
        else              snprintf(label, sizeof(label), "+");

        iOS::Fonts::Push(iOS::Fonts::Caption);
        ImVec2 ts = ImGui::CalcTextSize(label);
        iOS::Fonts::Pop(iOS::Fonts::Caption);

        float bw = ts.x + 16.0f * iOS::UI::scale;
        float minW = 30.0f * iOS::UI::scale;
        if (bw < minW) bw = minW;
        float bh = 21.0f * iOS::UI::scale;

        ImVec2 a(rightX - bw, cy - bh * 0.5f);
        ImVec2 b(rightX, cy + bh * 0.5f);

        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##bind", ImVec2(bw, bh));

        bool hovered = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
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
            float pulse = 0.5f + 0.5f * std::sin(s_bindClock * 6.0f);
            bg = iOS::Col::Alpha(iOS::Col::Blue, (0.18f + 0.18f * pulse) * entry);
            fg = iOS::Col::Alpha(iOS::Col::Blue, entry);
            dl->AddRect(a, b, iOS::Col::Alpha(iOS::Col::Blue,
                        (0.4f + 0.4f * pulse) * entry), 6.0f, 0, 1.2f);
        } else if (key > 0) {
            if (hov > 0.02f) iOS::Glow(dl, a, b, iOS::Col::Blue, 6.0f, hov * 0.5f, 3);
            bg = iOS::Col::Alpha(
                iOS::Col::Mix(iOS::Col::Fill, iOS::Col::BlueSoft, hov), entry);
            fg = iOS::Col::Alpha(
                iOS::Col::Mix(iOS::Col::Label2, iOS::Col::Blue, hov), entry);
        } else {
            // Unbound: nearly invisible until you go looking for it
            bg = iOS::Col::Alpha(iOS::Col::Fill, (0.3f + 0.7f * hov) * entry);
            fg = iOS::Col::Alpha(
                iOS::Col::Mix(iOS::Col::Label3, iOS::Col::Blue, hov), entry);
        }

        dl->AddRectFilled(a, b, bg, 6.0f);

        iOS::Fonts::Push(iOS::Fonts::Caption);
        dl->AddText(ImVec2(a.x + (bw - ts.x) * 0.5f, cy - ts.y * 0.5f), fg, label);
        iOS::Fonts::Pop(iOS::Fonts::Caption);
    }

    // Consume whatever the capture produced. Runs once per frame,
    // outside the row loop, because the module list may be rebuilt
    // between frames and the result has to survive that.
    static void PollBinding(float dt) {
        s_bindFade = iOS::Anim::ToStr("bindFade",
                                      s_binding.empty() ? 0.0f : 1.0f, 18.0f);

        if (s_bindResultFade > 0.0f) s_bindResultFade -= dt * 0.8f;
        if (s_bindResultFade < 0.0f) s_bindResultFade = 0.0f;

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

        // Confirmation, so you can see what you just did without
        // hunting for the row again.
        char lbl[40];
        if (newKey > 0) KeyCapture::Label(newKey, lbl, sizeof(lbl));
        else            snprintf(lbl, sizeof(lbl), "None");
        s_bindResult = target + "  \xE2\x86\x92  " + lbl;
        s_bindResultFade = 1.6f;

        // The bind is read on the client thread every tick, so it is
        // set there rather than written from under it.
        ModuleManager::QueueAction([target, newKey](JNIEnv*) {
            Module* mine = ModuleManager::Find(target);
            if (!mine) return;

            // Two modules on one key means one press fires both.
            if (newKey > 0) {
                for (auto& other : ModuleManager::GetModules()) {
                    if (other.get() == mine) continue;
                    if (other->GetKeybind() != newKey) continue;

                    other->SetKeybind(0);
                    iOS::Notify::Info(other->GetName() + " was unbound",
                        "That key now belongs to " + target + ".");
                }
            }
            mine->SetKeybind(newKey);
        });
    }

    // ---------------------------------------------------------
    // Bind overlay
    // ---------------------------------------------------------
    // Full screen, because a picker that is a small chip somewhere
    // in a list gives you no idea the client is now eating your
    // keyboard. Drawn on the foreground list, above everything.
    // ---------------------------------------------------------
    static void RenderBindOverlay() {
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        float sw = io.DisplaySize.x, sh = io.DisplaySize.y;

        // ---- The confirmation, after a bind lands ----
        if (s_bindResultFade > 0.001f && !s_bindResult.empty()) {
            float a = Clamp01(s_bindResultFade / 0.6f);
            iOS::Fonts::Push(iOS::Fonts::BodyBold);
            ImVec2 ts = ImGui::CalcTextSize(s_bindResult.c_str());
            float pad = 16.0f;
            float x = (sw - ts.x) * 0.5f;
            float y = sh * 0.5f - 40.0f - (1.0f - a) * 8.0f;

            dl->AddRectFilled(ImVec2(x - pad, y - pad * 0.55f),
                              ImVec2(x + ts.x + pad, y + ts.y + pad * 0.55f),
                              IM_COL32(20, 22, 28, (int)(215 * a)), 10.0f);
            dl->AddText(ImVec2(x, y),
                        iOS::Col::Alpha(IM_COL32(255, 255, 255, 255), a),
                        s_bindResult.c_str());
            iOS::Fonts::Pop(iOS::Fonts::BodyBold);
        }

        if (s_bindFade < 0.01f) return;
        float a = s_bindFade;

        // Scrim over everything, including the menu
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh),
                          IM_COL32(0, 0, 0, (int)(120 * a)));

        float pulse = 0.5f + 0.5f * std::sin(s_bindClock * 5.0f);

        float cw = 320.0f * iOS::UI::scale;
        float ch = 132.0f * iOS::UI::scale;
        ImVec2 c0((sw - cw) * 0.5f, (sh - ch) * 0.5f - (1.0f - a) * 10.0f);
        ImVec2 c1(c0.x + cw, c0.y + ch);

        iOS::Glow(dl, c0, c1, iOS::Col::Blue, iOS::M::CardRadius,
                  a * (0.5f + 0.5f * pulse), 4);
        dl->AddRectFilled(c0, c1, iOS::Col::Alpha(iOS::Col::Card, a),
                          iOS::M::CardRadius);
        dl->AddRect(c0, c1,
                    iOS::Col::Alpha(iOS::Col::Blue, a * (0.3f + 0.4f * pulse)),
                    iOS::M::CardRadius, 0, 1.5f);

        float y = c0.y + 24.0f * iOS::UI::scale;

        // Which module we are binding
        iOS::Fonts::Push(iOS::Fonts::Caption);
        ImVec2 ns = ImGui::CalcTextSize(s_binding.c_str());
        dl->AddText(ImVec2(c0.x + (cw - ns.x) * 0.5f, y),
                    iOS::Col::Alpha(iOS::Col::Blue, a), s_binding.c_str());
        iOS::Fonts::Pop(iOS::Fonts::Caption);
        y += ns.y + 10.0f * iOS::UI::scale;

        iOS::Fonts::Push(iOS::Fonts::Title);
        const char* head = "Press any key";
        ImVec2 hs = ImGui::CalcTextSize(head);
        dl->AddText(ImVec2(c0.x + (cw - hs.x) * 0.5f, y),
                    iOS::Col::Alpha(iOS::Col::Label, a), head);
        iOS::Fonts::Pop(iOS::Fonts::Title);
        y += hs.y + 12.0f * iOS::UI::scale;

        iOS::Fonts::Push(iOS::Fonts::Caption);
        const char* hint = "BACKSPACE for None      ESC to cancel";
        ImVec2 is = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(c0.x + (cw - is.x) * 0.5f, y),
                    iOS::Col::Alpha(iOS::Col::Label2, a), hint);
        iOS::Fonts::Pop(iOS::Fonts::Caption);
    }

    // ---------------------------------------------------------
    // Dimmed, vignetted world behind the sheet
    // ---------------------------------------------------------
    // Not a blur. A real gaussian needs the framebuffer in a texture
    // and a shader pass; this overlay is on the fixed-function GL2
    // backend precisely so it cannot disturb the game's state.
    // Faking it with stacked translucent quads costs fill rate and
    // looks like a smear, so the world is dimmed and pulled in with
    // a vignette instead, which reads as depth and is free.
    // ---------------------------------------------------------
    static void RenderScrim(float fade) {
        if (!iOS::UI::dim || fade < 0.01f) return;

        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        float sw = io.DisplaySize.x, sh = io.DisplaySize.y;
        int alpha = (int)(255.0f * iOS::UI::dimAmount * fade);
        if (alpha <= 0) return;

        dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh),
                          IM_COL32(8, 10, 14, alpha));

        if (!iOS::UI::vignette) return;

        // Four edge gradients. Cheaper and cleaner than a radial
        // texture, and at this strength nobody can tell.
        int edge = (int)(alpha * 0.85f);
        ImU32 dark = IM_COL32(0, 0, 0, edge);
        ImU32 clear = IM_COL32(0, 0, 0, 0);
        float bandY = sh * 0.34f;
        float bandX = sw * 0.26f;

        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(sw, bandY),
                                    dark, dark, clear, clear);
        dl->AddRectFilledMultiColor(ImVec2(0, sh - bandY), ImVec2(sw, sh),
                                    clear, clear, dark, dark);
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(bandX, sh),
                                    dark, clear, clear, dark);
        dl->AddRectFilledMultiColor(ImVec2(sw - bandX, 0), ImVec2(sw, sh),
                                    clear, dark, dark, clear);
    }

    // ---------------------------------------------------------
    // Advanced disclosure inside an open module
    // ---------------------------------------------------------
    static void AdvancedBlock(const std::shared_ptr<Module>& mod) {
        if (!mod->HasAdvanced()) return;

        bool& open = s_advanced[mod->GetName()];

        ImGui::Dummy(ImVec2(0, 2));

        float w = ImGui::GetContentRegionAvail().x;
        float h = 30.0f * iOS::UI::scale;
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
                          float entry, bool showCategory = false)
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

        // A hovered row grows a thin accent edge on the left. Cheap,
        // and it reads instantly as "this is the one you are on".
        if (hovT > 0.01f) {
            float barH = h * 0.55f * hovT;
            dl->AddRectFilled(ImVec2(p.x, cy - barH * 0.5f),
                              ImVec2(p.x + 3.0f * iOS::UI::scale, cy + barH * 0.5f),
                              iOS::Col::Alpha(accent, 0.9f * hovT * entry), 2.0f);
        }

        // The whole row nudges right under the pointer
        float nudge = iOS::UI::rowNudge ? hovT * 4.0f * iOS::UI::scale : 0.0f;

        // ---- Description card ----
        if (expandHover && iOS::UI::hoverInfo && !expanded) {
            s_hoverName = name;
            s_hoverDesc = mod->GetDescription();
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            s_hoverAnchor = ImVec2(win->Pos.x + win->Size.x + 12.0f, cy - 30.0f);
        }

        // Category dot brightens with the module
        float onT = iOS::Anim::To(ImGui::GetID("##dot"), on ? 1.0f : 0.0f, 14.0f);
        float dotX = p.x + nudge + iOS::M::RowPadX + 4.0f;

        if (onT > 0.02f) {
            iOS::GlowCircle(dl, ImVec2(dotX, cy), 4.0f,
                            accent, onT * entry * 0.9f, 3);
        }
        dl->AddCircleFilled(ImVec2(dotX, cy), 4.0f,
            iOS::Col::Alpha(accent, (0.28f + 0.72f * onT) * entry), 14);

        float textX = dotX + 14.0f;

        // A live status line, if the module has one, sits under the
        // name so you can read what it is doing without opening it.
        const char* status = on ? mod->StatusLine() : nullptr;
        const char* sub = status && status[0] ? status
                        : (showCategory ? CategoryName(mod->GetCategory()) : nullptr);
        ImU32 subCol = (status && status[0]) ? accent : iOS::Col::Label3;

        // The name is clipped rather than allowed to run under the
        // chip, which is what a long one used to do.
        float textRoom = (p.x + w - switchZone - 30.0f) - textX;
        if (textRoom < 40.0f) textRoom = 40.0f;
        ImGui::PushClipRect(ImVec2(textX, p.y),
                            ImVec2(textX + textRoom, p.y + h), true);

        if (sub) {
            ImVec2 ns = ImGui::CalcTextSize(name.c_str());
            dl->AddText(ImVec2(textX, cy - ns.y + 1.0f),
                        iOS::Col::Alpha(iOS::Col::Label, entry), name.c_str());

            iOS::Fonts::Push(iOS::Fonts::Caption);
            dl->AddText(ImVec2(textX, cy + 2.0f),
                        iOS::Col::Alpha(subCol, 0.85f * entry), sub);
            iOS::Fonts::Pop(iOS::Fonts::Caption);
        } else {
            ImVec2 ts = ImGui::CalcTextSize(name.c_str());
            dl->AddText(ImVec2(textX, cy - ts.y * 0.5f),
                        iOS::Col::Alpha(iOS::Col::Label, entry), name.c_str());
        }

        ImGui::PopClipRect();

        // ---- Keybind chip ----
        KeybindChip(mod, p.x + w - switchZone - 22.0f, cy, entry);

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

            mod->RenderSettings();
            AdvancedBlock(mod);

            ImGui::PopItemWidth();
            ImGui::Unindent(iOS::M::RowPadX);
            ImGui::Dummy(ImVec2(0, 6));

            iOS::EndCollapsible();
        }

        if (!last) iOS::RowSeparator(iOS::M::RowPadX + 18.0f);

        ImGui::PopID();
    }

    static void EmptyState(const char* msg) {
        ImGui::Dummy(ImVec2(0, 40));
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(p.x + (w - ts.x) * 0.5f, p.y), iOS::Col::Label3, msg);
        ImGui::Dummy(ImVec2(0, 30));
    }

    static void RenderList(const std::vector<std::shared_ptr<Module>>& mods,
                           bool showCategory)
    {
        if (mods.empty()) { EmptyState("Nothing here"); return; }

        iOS::BeginCard();
        for (size_t i = 0; i < mods.size(); i++)
            ModuleRow(mods[i], i + 1 == mods.size(), RowEntry((int)i), showCategory);
        iOS::EndCard();
        ImGui::Dummy(ImVec2(0, 6));
    }

    static void RenderTab(int tab) {
        std::vector<std::shared_ptr<Module>> mods;
        for (auto& m : ModuleManager::GetModules())
            if (InTab(m->GetCategory(), tab)) mods.push_back(m);

        if (mods.empty()) { EmptyState("Nothing here yet"); return; }

        int enabled = 0;
        for (auto& m : mods) if (m->IsEnabled()) enabled++;

        char head[64];
        snprintf(head, sizeof(head), "%d of %d active", enabled, (int)mods.size());
        iOS::SectionHeader(head);

        RenderList(mods, tab == TAB_MISC);
    }

    static void RenderSearch() {
        std::vector<std::shared_ptr<Module>> hits;
        for (auto& m : ModuleManager::GetModules())
            if (Matches(m, s_search)) hits.push_back(m);

        char head[80];
        if (hits.empty()) snprintf(head, sizeof(head), "No matches");
        else snprintf(head, sizeof(head), "%d result%s",
                      (int)hits.size(), hits.size() == 1 ? "" : "s");
        iOS::SectionHeader(head);

        if (hits.empty()) {
            EmptyState("Nothing matches that");
            iOS::Footnote("Search looks at the name, the description and the "
                          "category, so \"knockback\" finds Velocity.");
            return;
        }

        RenderList(hits, true);
    }

    // ---------------------------------------------------------
    // Interface tab
    // ---------------------------------------------------------
    static void RenderInterface() {
        using iOS::UI;

        iOS::SectionHeader("Accent");
        iOS::BeginCard();
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Indent(iOS::M::RowPadX);

        // Swatches. Faster to hit than a dropdown and you can see
        // every option at once, which is the whole point of colour.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float sz = 26.0f * UI::scale;
            float gap = 10.0f * UI::scale;

            for (int i = 0; i < UI::kAccentCount; i++) {
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();

                ImGui::InvisibleButton("##sw", ImVec2(sz, sz));
                bool hov = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) UI::accent = i;

                int saved = UI::accent;
                UI::accent = i;
                ImU32 col = UI::AccentColor();
                UI::accent = saved;

                ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
                float r = sz * 0.5f;

                bool sel = (UI::accent == i);
                float selT = iOS::Anim::To(ImGui::GetID("##selt"),
                                           sel ? 1.0f : 0.0f, 18.0f);
                float hovT = iOS::Anim::To(ImGui::GetID("##hovt"),
                                           hov ? 1.0f : 0.0f, 18.0f);

                if (selT > 0.02f || hovT > 0.02f)
                    iOS::GlowCircle(dl, c, r, col, selT * 0.9f + hovT * 0.4f, 3);

                dl->AddCircleFilled(c, r - 1.0f + hovT, col, 24);

                if (selT > 0.02f) {
                    dl->AddCircle(c, r + 3.0f * selT,
                                  iOS::Col::Alpha(col, selT), 24, 2.0f);
                }

                // The custom slot gets a slash so it is obviously
                // not just another blue.
                if (i == UI::kAccentCount - 1) {
                    dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.5f),
                                ImVec2(c.x + r * 0.5f, c.y - r * 0.5f),
                                IM_COL32(255, 255, 255, 190), 2.0f);
                }

                if (hov) ImGui::SetTooltip("%s", UI::AccentNames()[i]);

                ImGui::SetCursorScreenPos(ImVec2(p.x + sz + gap, p.y));
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, sz));
        }

        if (UI::accent == UI::kAccentCount - 1) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::ColorEdit4("Custom", UI::accentCustom,
                              ImGuiColorEditFlags_NoInputs |
                              ImGuiColorEditFlags_AlphaBar);
        }

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Unindent(iOS::M::RowPadX);
        iOS::EndCard();

        // ---- Layout ----
        iOS::SectionHeader("Layout");
        iOS::BeginCard();
        iOS::SliderRow("UI Scale", &UI::scale, 0.85f, 1.35f, "%.2fx",
                       "Everything grows together, not just the text.");
        iOS::RowSeparator();
        iOS::SliderRow("Rounded Corners", &UI::roundness, 0.0f, 1.6f, "%.2f",
                       "0 is square, 1 is the iOS radius.");
        iOS::EndCard();

        // ---- Motion ----
        iOS::SectionHeader("Motion");
        iOS::BeginCard();
        iOS::SliderRow("Animation Speed", &UI::animSpeed, 0.4f, 2.0f, "%.2fx",
                       "Scales every easing curve at once.");
        iOS::RowSeparator();
        iOS::SwitchRow("Opening Animation", &UI::openAnimation,
                       "Rows arrive one after another");
        iOS::RowSeparator();
        iOS::SwitchRow("Hover Nudge", &UI::rowNudge,
                       "The row under the pointer shifts slightly");
        iOS::RowSeparator();
        iOS::SwitchRow("Hover Descriptions", &UI::hoverInfo,
                       "Explains a module beside the panel");
        iOS::EndCard();

        // ---- Effects ----
        iOS::SectionHeader("Effects");
        iOS::BeginCard();
        iOS::SwitchRow("Glow", &UI::glow,
                       "Halo under switches, sliders and the accent");
        if (UI::glow) {
            iOS::RowSeparator();
            iOS::SliderRow("Glow Amount", &UI::glowAmount, 0.2f, 2.0f, "%.2fx");
        }
        iOS::RowSeparator();
        iOS::SwitchRow("Dim Background", &UI::dim,
                       "Darkens the world while the menu is open");
        if (UI::dim) {
            iOS::RowSeparator();
            iOS::SliderRow("Dim Amount", &UI::dimAmount, 0.0f, 0.8f, "%.2f");
            iOS::RowSeparator();
            iOS::SwitchRow("Vignette", &UI::vignette,
                           "Pulls the edges in around the panel");
        }
        iOS::EndCard();

        iOS::Footnote(
            "There is no blur, on purpose. A real one needs the framebuffer "
            "in a texture and a shader pass, and this overlay runs on the "
            "fixed-function backend so it cannot disturb the game's GL state. "
            "Faking it with stacked quads costs frames and looks like a smear.");

        ImGui::Dummy(ImVec2(0, 6));
        if (iOS::Button("Reset Interface",
                        ImGui::GetContentRegionAvail().x - iOS::M::RowPadX,
                        iOS::Col::Label2, false)) {
            UI::Reset();
            iOS::Notify::Info("Interface reset", "Back to the default look.");
        }
        ImGui::Dummy(ImVec2(0, 10));
    }

    // ---------------------------------------------------------
    // Header: app mark, title, live count, search, tabs
    // ---------------------------------------------------------
    static void RenderHeader(float width) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float s = iOS::UI::scale;
        const float h = 140.0f * s;

        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h),
                          iOS::Col::Card, iOS::M::SheetRound,
                          ImDrawFlags_RoundCornersTop);

        float ix = p.x + 20.0f * s, iy = p.y + 16.0f * s, is = 30.0f * s;
        iOS::Glow(dl, ImVec2(ix, iy), ImVec2(ix + is, iy + is),
                  iOS::Col::Blue, 8.0f, 0.6f, 3);
        dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + is, iy + is),
                          iOS::Col::Blue, 8.0f * iOS::UI::roundness);
        iOS::Fonts::Push(iOS::Fonts::BodyBold);
        ImVec2 ps = ImGui::CalcTextSize("P");
        dl->AddText(ImVec2(ix + (is - ps.x) * 0.5f, iy + (is - ps.y) * 0.5f),
                    iOS::Col::OnAccent, "P");
        iOS::Fonts::Pop(iOS::Fonts::BodyBold);

        iOS::Fonts::Push(iOS::Fonts::Title);
        dl->AddText(ImVec2(ix + is + 12.0f * s, iy - 3.0f), iOS::Col::Label, "Phantom");
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
        float bx = p.x + width - 20.0f * s - bw;
        float lit = Clamp01(shownActive);
        if (lit > 0.02f)
            iOS::Glow(dl, ImVec2(bx, iy + 4.0f), ImVec2(bx + bw, iy + 26.0f),
                      iOS::Col::Blue, 10.0f, lit * 0.5f, 3);
        dl->AddRectFilled(ImVec2(bx, iy + 4.0f), ImVec2(bx + bw, iy + 26.0f),
                          iOS::Col::Mix(iOS::Col::Fill, iOS::Col::BlueSoft, lit), 10.0f);
        dl->AddText(ImVec2(bx + 9.0f, iy + 4.0f + (22.0f - cs.y) * 0.5f),
                    iOS::Col::Mix(iOS::Col::Label2, iOS::Col::Blue, lit), cnt);
        iOS::Fonts::Pop(iOS::Fonts::Caption);

        // ---- Search ----
        ImGui::SetCursorScreenPos(ImVec2(p.x + 16.0f * s, p.y + 56.0f * s));
        iOS::SearchField("search", s_search, sizeof(s_search), width - 32.0f * s);

        // ---- Tabs ----
        // Greyed while searching, because results deliberately ignore
        // them and a live-looking control that does nothing is worse
        // than one that says it is asleep.
        bool searching = s_search[0] != '\0';
        ImGui::SetCursorScreenPos(ImVec2(p.x + 16.0f * s, p.y + 96.0f * s));
        if (searching) ImGui::BeginDisabled();
        iOS::Segmented("tabs", s_tabNames, TAB_COUNT, &s_tab, width - 32.0f * s);
        if (searching) ImGui::EndDisabled();

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
        // client is left listening for a key nobody will press.
        PollBinding(dt);

        s_fade = iOS::Anim::ToStr("menuFade",
                                  open ? 1.0f : 0.0f,
                                  iOS::UI::openAnimation ? 16.0f : 100.0f);

        if (!open && !s_binding.empty()) {
            KeyCapture::Cancel();
            s_binding.clear();
        }

        if (s_fade < 0.004f) {
            // Fully closed: forget the transient state, so reopening
            // is a clean screen rather than wherever you left off
            // mid-interaction.
            s_tabAge = 0.0f;
            s_lastTab = -1;
            s_hoverFade = 0.0f;
            s_search[0] = '\0';
            iOS::ConfigPanel::Reset();
            RenderBindOverlay();   // the confirmation may still be fading
            return;
        }

        // Push the user's look into the palette and the metrics. One
        // handful of multiplies; not worth tracking whether it moved.
        float savedFontScale = ImGui::GetIO().FontGlobalScale;
        iOS::UI::Apply();

        RenderScrim(s_fade);

        // Tab transition clock. Reset on a change, then run forward.
        // A change of search counts: the list is different, so it
        // should arrive the same way.
        static int lastSearchLen = -1;
        int qlen = (int)std::strlen(s_search);
        if (s_tab != s_lastTab || qlen != lastSearchLen) {
            s_lastTab = s_tab;
            lastSearchLen = qlen;
            s_tabAge = 0.0f;
        } else if (s_tabAge < 3.0f) {
            s_tabAge += dt;
        }

        // Nothing hovered this frame until a row claims it
        std::string prevHover = s_hoverName;
        s_hoverName.clear();

        const float s = iOS::UI::scale;
        ImGui::SetNextWindowSize(ImVec2(480, 600), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(120, 90), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(420 * s, 340 * s),
                                            ImVec2(820 * s, 1100 * s));
        ImGui::SetNextWindowBgAlpha(0.0f);   // the sheet draws its own

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_fade);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground;

        // Mid-fade the menu is on its way out: visible but not
        // clickable, or a stray click lands on a ghost. The bind
        // overlay locks it for the same reason.
        if (!open || !s_binding.empty()) flags |= ImGuiWindowFlags_NoInputs;

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
                              ImVec2(wp.x + ws.x + 2.0f,
                                     wp.y + ws.y + 4.0f + 6.0f * lift),
                              IM_COL32(0, 0, 0, (int)(58 * s_fade)),
                              iOS::M::SheetRound + 2.0f);
            dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                              iOS::Col::GroupedBg, iOS::M::SheetRound);

            RenderHeader(width);

            ImGui::BeginChild("##scroll", ImVec2(0, 0),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);

            // Content lifts into place behind the header
            if (iOS::UI::openAnimation)
                ImGui::Dummy(ImVec2(0, (1.0f - EaseOut(s_fade)) * 14.0f));

            ImGui::Indent(16.0f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 32.0f);

            if (s_search[0])            RenderSearch();
            else if (s_tab == TAB_UI)     RenderInterface();
            else if (s_tab == TAB_CONFIG) iOS::ConfigPanel::Render();
            else                          RenderTab(s_tab);

            ImGui::PopItemWidth();
            ImGui::Unindent(16.0f);
            ImGui::Dummy(ImVec2(0, 12));
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::PopStyleVar();

        // ---- Hover description ----
        // Crossfades: moving between rows fades the old one out and
        // the new one in rather than blinking the text.
        bool haveHover = iOS::UI::hoverInfo && !s_hoverName.empty()
                      && s_binding.empty();
        s_hoverFade = iOS::Anim::ToStr("hoverFade",
                                       haveHover ? 1.0f : 0.0f, 14.0f);
        if (s_hoverFade > 0.02f) {
            const std::string& title = haveHover ? s_hoverName : prevHover;
            if (!title.empty())
                iOS::HoverCard(s_hoverAnchor, title.c_str(), s_hoverDesc.c_str(),
                               s_hoverFade * s_fade);
        }

        RenderBindOverlay();

        ImGui::GetIO().FontGlobalScale = savedFontScale;
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
