#pragma once
#include <imgui.h>
#include <cstdio>
#include "../modules/module_manager.h"
#include "../render/backtrack_vis.h"
#include "../config/profiles.h"

// =================================================================
// Menu
// =================================================================
// Runs entirely on the render thread, so it must never call JNI.
// Toggles and profile loads are queued to ModuleManager and applied
// on the client thread where a valid JNIEnv exists.
// =================================================================

class Menu {
public:
    inline static int s_currentTab = 0;

    inline static const char* s_tabNames[6] = {
        "Combat", "Movement", "Visual", "Player", "Misc", "Configs"
    };
    inline static ModuleCategory s_tabCategories[5] = {
        ModuleCategory::COMBAT,
        ModuleCategory::MOVEMENT,
        ModuleCategory::VISUAL,
        ModuleCategory::PLAYER,
        ModuleCategory::MISC
    };

    static void ApplyTheme() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* c = style.Colors;

        style.WindowRounding    = 12.0f;
        style.FrameRounding     = 8.0f;
        style.GrabRounding      = 6.0f;
        style.TabRounding       = 6.0f;
        style.ChildRounding     = 8.0f;
        style.PopupRounding     = 8.0f;
        style.ScrollbarRounding = 6.0f;

        style.WindowPadding    = ImVec2(16, 16);
        style.FramePadding     = ImVec2(12, 6);
        style.ItemSpacing      = ImVec2(10, 8);
        style.ItemInnerSpacing = ImVec2(8, 4);
        style.ScrollbarSize    = 10.0f;
        style.GrabMinSize      = 10.0f;
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize  = 0.0f;

        const ImVec4 bg        = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
        const ImVec4 surface   = ImVec4(0.11f, 0.11f, 0.16f, 1.00f);
        const ImVec4 elevated  = ImVec4(0.14f, 0.14f, 0.20f, 1.00f);
        const ImVec4 accent    = ImVec4(0.45f, 0.35f, 0.95f, 1.00f);
        const ImVec4 accentDim = ImVec4(0.35f, 0.25f, 0.75f, 1.00f);
        const ImVec4 textPri   = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
        const ImVec4 textSec   = ImVec4(0.55f, 0.55f, 0.62f, 1.00f);

        c[ImGuiCol_WindowBg]            = bg;
        c[ImGuiCol_ChildBg]             = surface;
        c[ImGuiCol_PopupBg]             = surface;
        c[ImGuiCol_Border]              = ImVec4(0.18f, 0.18f, 0.25f, 1.00f);
        c[ImGuiCol_FrameBg]             = elevated;
        c[ImGuiCol_FrameBgHovered]      = ImVec4(0.18f, 0.18f, 0.26f, 1.00f);
        c[ImGuiCol_FrameBgActive]       = ImVec4(0.22f, 0.22f, 0.30f, 1.00f);
        c[ImGuiCol_TitleBg]             = surface;
        c[ImGuiCol_TitleBgActive]       = surface;
        c[ImGuiCol_MenuBarBg]           = surface;
        c[ImGuiCol_ScrollbarBg]         = bg;
        c[ImGuiCol_ScrollbarGrab]       = elevated;
        c[ImGuiCol_ScrollbarGrabHovered]= accentDim;
        c[ImGuiCol_ScrollbarGrabActive] = accent;
        c[ImGuiCol_CheckMark]           = accent;
        c[ImGuiCol_SliderGrab]          = accentDim;
        c[ImGuiCol_SliderGrabActive]    = accent;
        c[ImGuiCol_Button]              = elevated;
        c[ImGuiCol_ButtonHovered]       = accentDim;
        c[ImGuiCol_ButtonActive]        = accent;
        c[ImGuiCol_Header]              = elevated;
        c[ImGuiCol_HeaderHovered]       = ImVec4(0.18f, 0.18f, 0.26f, 1.00f);
        c[ImGuiCol_HeaderActive]        = accentDim;
        c[ImGuiCol_Separator]           = ImVec4(0.18f, 0.18f, 0.25f, 0.50f);
        c[ImGuiCol_Tab]                 = surface;
        c[ImGuiCol_TabHovered]          = accentDim;
        // 1.90 calls this TabActive. TabSelected only exists in 1.91+.
        c[ImGuiCol_TabActive]           = accent;
        c[ImGuiCol_TabUnfocused]        = surface;
        c[ImGuiCol_TabUnfocusedActive]  = accentDim;
        c[ImGuiCol_Text]                = textPri;
        c[ImGuiCol_TextDisabled]        = textSec;
        c[ImGuiCol_ResizeGrip]          = accent;
    }

    static void KeyLabel(int key, char* out, size_t n) {
        if (key >= 'A' && key <= 'Z')  snprintf(out, n, "[%c]", (char)key);
        else if (key == VK_CONTROL)    snprintf(out, n, "[CTRL]");
        else if (key == VK_SHIFT)      snprintf(out, n, "[SHIFT]");
        else if (key == VK_MENU)       snprintf(out, n, "[ALT]");
        else if (key > 0)              snprintf(out, n, "[0x%X]", key);
        else                           out[0] = '\0';
    }

    static void RenderModuleList(ModuleCategory cat) {
        auto modules = ModuleManager::GetModulesByCategory(cat);

        if (modules.empty()) {
            ImGui::TextDisabled("No modules in this category.");
            return;
        }

        for (auto& mod : modules) {
            ImGui::PushID(mod.get());

            bool enabled = mod->IsEnabled();
            if (ImGui::Checkbox("##toggle", &enabled)) {
                // Do not flip state here: OnEnable/OnDisable need a
                // JNIEnv, so hand it to the client thread.
                ModuleManager::QueueToggle(mod.get());
            }

            ImGui::SameLine();
            bool open = ImGui::CollapsingHeader(mod->GetName().c_str());

            int key = mod->GetKeybind();
            if (key > 0) {
                char buf[16];
                KeyLabel(key, buf, sizeof(buf));
                ImVec2 sz = ImGui::CalcTextSize(buf);
                ImGui::SameLine(ImGui::GetContentRegionMax().x - sz.x - 4.0f);
                ImGui::TextDisabled("%s", buf);
            }

            if (open) {
                ImGui::Indent(12.0f);
                ImGui::TextDisabled("%s", mod->GetDescription().c_str());
                ImGui::Spacing();
                mod->RenderSettings();
                ImGui::Spacing();
                ImGui::Unindent(12.0f);
            }

            ImGui::PopID();
        }
    }

    static void RenderConfigs() {
        ImGui::TextWrapped(
            "A profile turns the right modules on, forces the dangerous ones "
            "off, and sets values tuned for that server's anticheat.");
        ImGui::Spacing();

        auto profiles = Profiles::All();
        for (size_t i = 0; i < profiles.size(); i++) {
            ImGui::PushID((int)i);

            ImGui::TextColored(ImVec4(0.45f, 0.35f, 0.95f, 1.f), "%s", profiles[i].name);
            ImGui::TextDisabled("%s", profiles[i].description);

            if (ImGui::Button("Load", ImVec2(120, 0))) {
                Profile p = profiles[i];
                ModuleManager::QueueAction([p](JNIEnv* env) {
                    Profiles::Apply(p, env);
                });
            }

            ImGui::Spacing();
            ImGui::PopID();
        }

        const std::string& report = Profiles::LastReport();
        if (!report.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f), "%s", report.c_str());
        }
    }

    static void Render() {
        ImGui::SetNextWindowSize(ImVec2(580, 460), ImGuiCond_FirstUseEver);
        ImGui::Begin("Phantom Client v2.4.1", nullptr, ImGuiWindowFlags_NoCollapse);

        if (ImGui::BeginTabBar("##tabs")) {
            for (int i = 0; i < 6; i++) {
                if (ImGui::BeginTabItem(s_tabNames[i])) {
                    s_currentTab = i;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::BeginChild("##body", ImVec2(0, 0), false);

        if (s_currentTab == 5) RenderConfigs();
        else                   RenderModuleList(s_tabCategories[s_currentTab]);

        ImGui::EndChild();
        ImGui::End();
    }

    // Active-module list, top right
    static void RenderHUD() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##hud", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing);

        ImGui::TextColored(ImVec4(0.45f, 0.35f, 0.95f, 1.0f), "Phantom");

        for (auto& mod : ModuleManager::GetModules()) {
            if (!mod->IsEnabled()) continue;
            ImVec4 col;
            switch (mod->GetCategory()) {
                case ModuleCategory::COMBAT:   col = ImVec4(1.0f, 0.40f, 0.40f, 1.0f); break;
                case ModuleCategory::MOVEMENT: col = ImVec4(0.40f, 0.70f, 1.00f, 1.0f); break;
                case ModuleCategory::VISUAL:   col = ImVec4(0.70f, 0.50f, 1.00f, 1.0f); break;
                case ModuleCategory::PLAYER:   col = ImVec4(0.40f, 1.00f, 0.60f, 1.0f); break;
                default:                       col = ImVec4(1.00f, 0.80f, 0.30f, 1.0f); break;
            }
            ImGui::TextColored(col, "%s", mod->GetName().c_str());
        }

        ImGui::End();
    }

    // World overlays, drawn every frame regardless of menu state.
    // Backtrack draws after the ESP so the held position sits on top
    // of the box marking where the server actually has the player.
    static void RenderOverlays() {
        auto esp = ModuleManager::GetESP();
        if (esp) esp->RenderESP();

        auto bt = ModuleManager::GetBacktrack();
        if (bt) BacktrackVis::Render(bt.get());
    }
};
