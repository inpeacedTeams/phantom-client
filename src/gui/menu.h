#pragma once
#include <imgui.h>
#include <string>
#include "../modules/module_manager.h"

class Menu {
public:
    inline static int s_currentTab = 0;
    inline static const char* s_tabNames[] = {
        "Combat", "Movement", "Visual", "Player", "Misc"
    };
    inline static ModuleCategory s_tabCategories[] = {
        ModuleCategory::COMBAT,
        ModuleCategory::MOVEMENT,
        ModuleCategory::VISUAL,
        ModuleCategory::PLAYER,
        ModuleCategory::MISC
    };

    static void ApplyTheme() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        // Rounded, clean, iOS-inspired
        style.WindowRounding = 12.0f;
        style.FrameRounding = 8.0f;
        style.GrabRounding = 6.0f;
        style.TabRounding = 6.0f;
        style.ChildRounding = 8.0f;
        style.PopupRounding = 8.0f;
        style.ScrollbarRounding = 6.0f;

        style.WindowPadding = ImVec2(16, 16);
        style.FramePadding = ImVec2(12, 6);
        style.ItemSpacing = ImVec2(10, 8);
        style.ItemInnerSpacing = ImVec2(8, 4);
        style.ScrollbarSize = 10.0f;
        style.GrabMinSize = 10.0f;

        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;

        // Dark theme with purple accent
        ImVec4 bg       = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
        ImVec4 surface   = ImVec4(0.11f, 0.11f, 0.16f, 1.00f);
        ImVec4 elevated  = ImVec4(0.14f, 0.14f, 0.20f, 1.00f);
        ImVec4 accent    = ImVec4(0.45f, 0.35f, 0.95f, 1.00f);
        ImVec4 accentDim = ImVec4(0.35f, 0.25f, 0.75f, 1.00f);
        ImVec4 textPri   = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
        ImVec4 textSec   = ImVec4(0.55f, 0.55f, 0.62f, 1.00f);

        colors[ImGuiCol_WindowBg]       = bg;
        colors[ImGuiCol_ChildBg]        = surface;
        colors[ImGuiCol_PopupBg]        = surface;
        colors[ImGuiCol_Border]         = ImVec4(0.18f, 0.18f, 0.25f, 1.00f);
        colors[ImGuiCol_FrameBg]        = elevated;
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.26f, 1.00f);
        colors[ImGuiCol_FrameBgActive]  = ImVec4(0.22f, 0.22f, 0.30f, 1.00f);
        colors[ImGuiCol_TitleBg]        = surface;
        colors[ImGuiCol_TitleBgActive]  = surface;
        colors[ImGuiCol_MenuBarBg]      = surface;
        colors[ImGuiCol_ScrollbarBg]    = bg;
        colors[ImGuiCol_ScrollbarGrab]  = elevated;
        colors[ImGuiCol_ScrollbarGrabHovered] = accentDim;
        colors[ImGuiCol_ScrollbarGrabActive]  = accent;
        colors[ImGuiCol_CheckMark]      = accent;
        colors[ImGuiCol_SliderGrab]     = accentDim;
        colors[ImGuiCol_SliderGrabActive] = accent;
        colors[ImGuiCol_Button]         = elevated;
        colors[ImGuiCol_ButtonHovered]   = accentDim;
        colors[ImGuiCol_ButtonActive]    = accent;
        colors[ImGuiCol_Header]         = elevated;
        colors[ImGuiCol_HeaderHovered]   = ImVec4(0.18f, 0.18f, 0.26f, 1.00f);
        colors[ImGuiCol_HeaderActive]    = accentDim;
        colors[ImGuiCol_Separator]      = ImVec4(0.18f, 0.18f, 0.25f, 0.50f);
        colors[ImGuiCol_Tab]            = surface;
        colors[ImGuiCol_TabHovered]     = accentDim;
        colors[ImGuiCol_TabSelected]    = accent;
        colors[ImGuiCol_Text]           = textPri;
        colors[ImGuiCol_TextDisabled]   = textSec;
        colors[ImGuiCol_ResizeGrip]     = accent;
    }

    static void Render() {
        ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
        ImGui::Begin("Phantom Client v2.4.1", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

        // Tab bar
        if (ImGui::BeginTabBar("##tabs")) {
            for (int i = 0; i < 5; i++) {
                if (ImGui::BeginTabItem(s_tabNames[i])) {
                    s_currentTab = i;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();

        // Module list for current tab
        auto modules = ModuleManager::GetModulesByCategory(s_tabCategories[s_currentTab]);

        ImGui::BeginChild("##modules", ImVec2(0, 0), false);
        for (auto& mod : modules) {
            ImGui::PushID(mod->GetName().c_str());

            // Module header with toggle
            bool enabled = mod->IsEnabled();
            bool header = ImGui::CollapsingHeader(mod->GetName().c_str(),
                ImGuiTreeNodeFlags_AllowOverlap);

            // Toggle checkbox on the right
            ImGui::SameLine(ImGui::GetWindowWidth() - 60);
            if (ImGui::Checkbox("##toggle", &enabled)) {
                // We need env here - in real impl, pass it or use global
                // For now, just toggle the flag
                mod->SetEnabled(enabled, nullptr);
            }

            // Keybind display
            if (mod->GetKeybind() > 0) {
                ImGui::SameLine(ImGui::GetWindowWidth() - 100);
                char keyStr[16];
                if (mod->GetKeybind() >= 'A' && mod->GetKeybind() <= 'Z') {
                    snprintf(keyStr, sizeof(keyStr), "[%c]", (char)mod->GetKeybind());
                } else {
                    snprintf(keyStr, sizeof(keyStr), "[0x%X]", mod->GetKeybind());
                }
                ImGui::TextDisabled("%s", keyStr);
            }

            if (header) {
                ImGui::TextDisabled("%s", mod->GetDescription().c_str());
                ImGui::Spacing();
                mod->RenderSettings();
                ImGui::Spacing();
            }

            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::End();
    }

    static void RenderHUD() {
        // Active modules list in top-right corner
        ImGui::SetNextWindowPos(ImVec2(
            ImGui::GetIO().DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##hud", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);

        // Client name
        ImGui::TextColored(ImVec4(0.45f, 0.35f, 0.95f, 1.0f), "Phantom");
        ImGui::Separator();

        // List enabled modules
        for (auto& mod : ModuleManager::GetModules()) {
            if (mod->IsEnabled()) {
                ImVec4 color;
                switch (mod->GetCategory()) {
                    case ModuleCategory::COMBAT:   color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); break;
                    case ModuleCategory::MOVEMENT:  color = ImVec4(0.4f, 0.7f, 1.0f, 1.0f); break;
                    case ModuleCategory::VISUAL:    color = ImVec4(0.7f, 0.5f, 1.0f, 1.0f); break;
                    case ModuleCategory::PLAYER:    color = ImVec4(0.4f, 1.0f, 0.6f, 1.0f); break;
                    case ModuleCategory::MISC:      color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f); break;
                }
                ImGui::TextColored(color, "%s", mod->GetName().c_str());
            }
        }

        ImGui::End();
    }
};
