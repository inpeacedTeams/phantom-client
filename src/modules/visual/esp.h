#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include <imgui.h>
#include <gl/GL.h>
#include <cmath>
#include <cstdio>
#include <vector>

// =================================================================
// ESP — Full implementation with OpenGL screen projection
// =================================================================
// 1. Gets all players from EntityList
// 2. Projects 3D bounding box corners to 2D screen via OpenGL
// 3. Draws 2D boxes, health bars, names, distance via ImGui
// =================================================================

// We implement gluProject manually to avoid linking glu32.lib
static bool WorldToScreen(double worldX, double worldY, double worldZ,
    const double modelview[16], const double projection[16], const int viewport[4],
    double* screenX, double* screenY, double* screenZ)
{
    // Transform by modelview
    double clip[4];
    clip[0] = modelview[0]*worldX + modelview[4]*worldY + modelview[8]*worldZ + modelview[12];
    clip[1] = modelview[1]*worldX + modelview[5]*worldY + modelview[9]*worldZ + modelview[13];
    clip[2] = modelview[2]*worldX + modelview[6]*worldY + modelview[10]*worldZ + modelview[14];
    clip[3] = modelview[3]*worldX + modelview[7]*worldY + modelview[11]*worldZ + modelview[15];

    // Transform by projection
    double ndc[4];
    ndc[0] = projection[0]*clip[0] + projection[4]*clip[1] + projection[8]*clip[2] + projection[12]*clip[3];
    ndc[1] = projection[1]*clip[0] + projection[5]*clip[1] + projection[9]*clip[2] + projection[13]*clip[3];
    ndc[2] = projection[2]*clip[0] + projection[6]*clip[1] + projection[10]*clip[2] + projection[14]*clip[3];
    ndc[3] = projection[3]*clip[0] + projection[7]*clip[1] + projection[11]*clip[2] + projection[15]*clip[3];

    if (std::abs(ndc[3]) < 0.001) return false; // Behind camera

    // Perspective divide
    ndc[0] /= ndc[3];
    ndc[1] /= ndc[3];
    ndc[2] /= ndc[3];

    // Map to window coordinates
    *screenX = viewport[0] + (viewport[2] * (ndc[0] + 1.0)) / 2.0;
    *screenY = viewport[1] + (viewport[3] * (ndc[1] + 1.0)) / 2.0;
    *screenZ = (ndc[2] + 1.0) / 2.0;

    // Flip Y (OpenGL origin is bottom-left, ImGui is top-left)
    *screenY = viewport[3] - *screenY;

    return ndc[3] > 0.0; // Only visible if in front of camera
}

class ESP : public Module {
private:
    bool m_showBox = true;
    bool m_showHealthBar = true;
    bool m_showDistance = true;
    bool m_showName = true;
    bool m_showArmor = false;
    bool m_showSnaplines = false;
    int m_boxStyle = 0;          // 0 = 2D Corners, 1 = 2D Full
    float m_maxRange = 64.0f;
    float m_lineThickness = 1.5f;

    // Colors (RGBA)
    float m_colorEnemy[4]  = { 1.0f, 0.3f, 0.3f, 1.0f };
    float m_colorFriend[4] = { 0.3f, 1.0f, 0.3f, 1.0f };

    // Cached GL matrices (captured during SwapBuffers hook)
    double m_modelview[16] = {};
    double m_projection[16] = {};
    int m_viewport[4] = {};
    bool m_matricesCaptured = false;

    // Cached entity data from last tick
    std::vector<EntityInfo> m_entities;

public:
    ESP() : Module("ESP", "Highlight players through walls", ModuleCategory::VISUAL, 'X') {}

    void OnTick(JNIEnv* env) override {
        if (!m_enabled) return;

        // Initialize EntityList if needed
        EntityList::Init(env);

        // Get all nearby players
        m_entities = EntityList::GetPlayers(env, m_maxRange);
    }

    // Call this from the GL hook BEFORE ImGui rendering starts
    // (while we're still in the game's GL context)
    void CaptureMatrices() {
        glGetDoublev(GL_MODELVIEW_MATRIX, m_modelview);
        glGetDoublev(GL_PROJECTION_MATRIX, m_projection);
        glGetIntegerv(GL_VIEWPORT, m_viewport);
        m_matricesCaptured = true;
    }

    // Call this during ImGui rendering (from HUD render)
    void RenderESP(JNIEnv* env) {
        if (!m_enabled || !m_matricesCaptured) return;
        if (m_entities.empty()) return;

        auto* drawList = ImGui::GetBackgroundDrawList();
        if (!drawList) return;

        // RenderManager offset (camera position)
        // In Minecraft, entities are rendered relative to RenderManager pos.
        // We need to subtract it to get screen-relative coords.
        // For now, we use the player's position as camera origin,
        // since GL modelview already includes the camera transform.

        for (auto& e : m_entities) {
            // Interpolated position (smooth between ticks)
            // partialTicks is handled by the rendering pipeline,
            // but for our overlay we use prevPos for smoother rendering
            double interpX = e.prevPosX + (e.posX - e.prevPosX) * 0.5;
            double interpY = e.prevPosY + (e.posY - e.prevPosY) * 0.5;
            double interpZ = e.prevPosZ + (e.posZ - e.prevPosZ) * 0.5;

            // Entity bounding box (player is 0.6 wide, 1.8 tall)
            double halfWidth = 0.3;
            double height = 1.8;

            // Project 8 corners of bounding box to find 2D bounds
            double minScreenX = 99999, minScreenY = 99999;
            double maxScreenX = -99999, maxScreenY = -99999;
            bool anyVisible = false;

            double corners[8][3] = {
                { interpX - halfWidth, interpY,          interpZ - halfWidth },
                { interpX + halfWidth, interpY,          interpZ - halfWidth },
                { interpX - halfWidth, interpY,          interpZ + halfWidth },
                { interpX + halfWidth, interpY,          interpZ + halfWidth },
                { interpX - halfWidth, interpY + height, interpZ - halfWidth },
                { interpX + halfWidth, interpY + height, interpZ - halfWidth },
                { interpX - halfWidth, interpY + height, interpZ + halfWidth },
                { interpX + halfWidth, interpY + height, interpZ + halfWidth },
            };

            for (int i = 0; i < 8; i++) {
                double sx, sy, sz;
                if (WorldToScreen(corners[i][0], corners[i][1], corners[i][2],
                    m_modelview, m_projection, m_viewport, &sx, &sy, &sz))
                {
                    if (sx < minScreenX) minScreenX = sx;
                    if (sy < minScreenY) minScreenY = sy;
                    if (sx > maxScreenX) maxScreenX = sx;
                    if (sy > maxScreenY) maxScreenY = sy;
                    anyVisible = true;
                }
            }

            if (!anyVisible) continue;

            // Sanity check screen bounds
            float left   = (float)minScreenX;
            float top    = (float)minScreenY;
            float right  = (float)maxScreenX;
            float bottom = (float)maxScreenY;
            float boxW   = right - left;
            float boxH   = bottom - top;

            if (boxW < 2 || boxH < 2) continue;   // Too small
            if (boxW > 2000 || boxH > 2000) continue; // Too big (glitch)

            // Choose color based on health
            ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(
                ImVec4(m_colorEnemy[0], m_colorEnemy[1], m_colorEnemy[2], m_colorEnemy[3]));

            // Hurt flash: briefly go white when hit
            if (e.hurtTime > 0) {
                boxColor = IM_COL32(255, 255, 255, 220);
            }

            // ==================
            // Draw Box
            // ==================
            if (m_showBox) {
                if (m_boxStyle == 0) {
                    // 2D Corners style
                    float cornerLen = std::min(boxW, boxH) * 0.25f;
                    cornerLen = std::max(cornerLen, 4.0f);

                    // Top-left
                    drawList->AddLine(ImVec2(left, top), ImVec2(left + cornerLen, top), boxColor, m_lineThickness);
                    drawList->AddLine(ImVec2(left, top), ImVec2(left, top + cornerLen), boxColor, m_lineThickness);
                    // Top-right
                    drawList->AddLine(ImVec2(right, top), ImVec2(right - cornerLen, top), boxColor, m_lineThickness);
                    drawList->AddLine(ImVec2(right, top), ImVec2(right, top + cornerLen), boxColor, m_lineThickness);
                    // Bottom-left
                    drawList->AddLine(ImVec2(left, bottom), ImVec2(left + cornerLen, bottom), boxColor, m_lineThickness);
                    drawList->AddLine(ImVec2(left, bottom), ImVec2(left, bottom - cornerLen), boxColor, m_lineThickness);
                    // Bottom-right
                    drawList->AddLine(ImVec2(right, bottom), ImVec2(right - cornerLen, bottom), boxColor, m_lineThickness);
                    drawList->AddLine(ImVec2(right, bottom), ImVec2(right, bottom - cornerLen), boxColor, m_lineThickness);

                } else {
                    // 2D Full box
                    drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), boxColor, 0, 0, m_lineThickness);
                }

                // Dark outline for readability
                ImU32 outlineColor = IM_COL32(0, 0, 0, 100);
                drawList->AddRect(
                    ImVec2(left - 1, top - 1), ImVec2(right + 1, bottom + 1),
                    outlineColor, 0, 0, 1.0f);
            }

            // ==================
            // Health Bar (left side)
            // ==================
            if (m_showHealthBar) {
                float barWidth = 3.0f;
                float barLeft = left - barWidth - 3;
                float healthPct = e.maxHealth > 0 ? (e.health / e.maxHealth) : 0;
                healthPct = std::max(0.f, std::min(1.f, healthPct));

                float barHeight = boxH * healthPct;
                float barTop = bottom - barHeight;

                // Background
                drawList->AddRectFilled(
                    ImVec2(barLeft, top), ImVec2(barLeft + barWidth, bottom),
                    IM_COL32(0, 0, 0, 150));

                // Health fill (green -> yellow -> red)
                ImU32 healthColor;
                if (healthPct > 0.5f) {
                    float t = (healthPct - 0.5f) * 2.0f;
                    healthColor = IM_COL32(
                        (int)((1.f - t) * 255), 255, 0, 255);
                } else {
                    float t = healthPct * 2.0f;
                    healthColor = IM_COL32(
                        255, (int)(t * 255), 0, 255);
                }

                drawList->AddRectFilled(
                    ImVec2(barLeft, barTop), ImVec2(barLeft + barWidth, bottom),
                    healthColor);

                // Health text
                char hpText[16];
                snprintf(hpText, sizeof(hpText), "%.0f", e.health);
                drawList->AddText(
                    ImVec2(barLeft - 2, barTop - 12),
                    IM_COL32(255, 255, 255, 220), hpText);
            }

            // ==================
            // Name Tag
            // ==================
            if (m_showName && !e.name.empty()) {
                ImVec2 textSize = ImGui::CalcTextSize(e.name.c_str());
                float nameX = left + (boxW - textSize.x) * 0.5f;
                float nameY = top - textSize.y - 3;

                // Background
                drawList->AddRectFilled(
                    ImVec2(nameX - 3, nameY - 1),
                    ImVec2(nameX + textSize.x + 3, nameY + textSize.y + 1),
                    IM_COL32(0, 0, 0, 140), 3.0f);

                drawList->AddText(
                    ImVec2(nameX, nameY),
                    IM_COL32(255, 255, 255, 255), e.name.c_str());
            }

            // ==================
            // Distance
            // ==================
            if (m_showDistance) {
                char distText[32];
                snprintf(distText, sizeof(distText), "%.1fm", e.distanceToPlayer);
                ImVec2 textSize = ImGui::CalcTextSize(distText);
                float distX = left + (boxW - textSize.x) * 0.5f;

                drawList->AddText(
                    ImVec2(distX, bottom + 3),
                    IM_COL32(200, 200, 200, 200), distText);
            }

            // ==================
            // Snaplines
            // ==================
            if (m_showSnaplines) {
                float screenCenterX = m_viewport[2] / 2.0f;
                float screenBottom  = (float)m_viewport[3];
                float targetCenterX = left + boxW * 0.5f;
                float targetBottom  = bottom;

                // Color by distance
                float distPct = (float)(e.distanceToPlayer / m_maxRange);
                distPct = std::max(0.f, std::min(1.f, distPct));
                ImU32 lineColor = IM_COL32(
                    (int)(distPct * 255), (int)((1.f - distPct) * 255), 0, 180);

                drawList->AddLine(
                    ImVec2(screenCenterX, screenBottom),
                    ImVec2(targetCenterX, targetBottom),
                    lineColor, 1.0f);
            }
        }
    }

    void RenderSettings() override {
        const char* styles[] = { "2D Corners", "2D Full" };
        ImGui::Combo("Box Style", &m_boxStyle, styles, 2);
        ImGui::Checkbox("Box", &m_showBox);
        ImGui::Checkbox("Health Bar", &m_showHealthBar);
        ImGui::Checkbox("Name", &m_showName);
        ImGui::Checkbox("Distance", &m_showDistance);
        ImGui::Checkbox("Snaplines", &m_showSnaplines);
        ImGui::SliderFloat("Max Range", &m_maxRange, 16.f, 128.f, "%.0f");
        ImGui::SliderFloat("Line Thickness", &m_lineThickness, 1.f, 4.f, "%.1f");
        ImGui::ColorEdit4("Enemy Color", m_colorEnemy);
        ImGui::ColorEdit4("Friend Color", m_colorFriend);
    }
};
