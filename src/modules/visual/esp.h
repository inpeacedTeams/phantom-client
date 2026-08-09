#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>
#include <gl/GL.h>

class ESP : public Module {
private:
    bool m_showBox = true;
    bool m_showHealthBar = true;
    bool m_showDistance = true;
    bool m_showName = true;
    int m_boxStyle = 0; // 0 = 2D Corners, 1 = 2D Full, 2 = 3D
    float m_maxRange = 64.0f;

    // Colors (RGBA)
    float m_colorEnemy[4]  = { 1.0f, 0.3f, 0.3f, 1.0f };
    float m_colorFriend[4] = { 0.3f, 1.0f, 0.3f, 1.0f };

public:
    ESP() : Module("ESP", "Highlight players through walls", ModuleCategory::VISUAL, 'X') {}

    void OnTick(JNIEnv* env) override {
        // ESP rendering happens in the ImGui render loop, not here.
        // This tick is used to update cached entity data.
    }

    // Called from Menu::RenderHUD() during ImGui frame
    void RenderESP(JNIEnv* env) {
        if (!m_enabled) return;

        jobject player = Minecraft::GetPlayer(env);
        jobject world = Minecraft::GetWorld(env);
        if (!player || !world) return;

        // TODO: Full implementation would:
        // 1. Get playerEntities list from world
        // 2. For each entity (skip self):
        //    a. Get entity position (interpolated with partialTicks)
        //    b. Project 3D position to 2D screen via OpenGL matrices
        //    c. Draw box, health bar, name tag, distance
        //
        // Screen projection via OpenGL:
        // - Get modelview matrix: glGetDoublev(GL_MODELVIEW_MATRIX, modelview)
        // - Get projection matrix: glGetDoublev(GL_PROJECTION_MATRIX, projection)
        // - Get viewport: glGetIntegerv(GL_VIEWPORT, viewport)
        // - gluProject(worldX, worldY, worldZ, modelview, projection, viewport, &screenX, &screenY, &screenZ)
        //
        // Then use ImGui::GetBackgroundDrawList() to draw 2D overlays:
        //
        // auto* drawList = ImGui::GetBackgroundDrawList();
        // drawList->AddRect(...);
        // drawList->AddText(...);
        // drawList->AddRectFilled(...); // for health bar
    }

    void RenderSettings() override {
        const char* styles[] = { "2D Corners", "2D Full", "3D" };
        ImGui::Combo("Box Style", &m_boxStyle, styles, 3);
        ImGui::Checkbox("Health Bar", &m_showHealthBar);
        ImGui::Checkbox("Distance", &m_showDistance);
        ImGui::Checkbox("Name", &m_showName);
        ImGui::SliderFloat("Max Range", &m_maxRange, 16.0f, 128.0f, "%.0f");
        ImGui::ColorEdit4("Enemy Color", m_colorEnemy);
        ImGui::ColorEdit4("Friend Color", m_colorFriend);
    }
};
