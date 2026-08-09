#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../render/camera.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <mutex>
#include <chrono>

// =================================================================
// ESP
// =================================================================
// Projection comes from the shared Camera, which rebuilds the view
// transform from eye position, yaw, pitch and FOV. Reading the GL
// matrices from the swap hook does not work: by then Minecraft has
// finished the world AND GUI passes, so the matrix on the stack is
// the orthographic GUI one and world coordinates project to junk.
//
// THREADING
// OnTick runs on the client thread and publishes a plain-data
// snapshot under a mutex. RenderESP runs on the render thread and
// only reads it, so it never touches JNI.
// =================================================================

class ESP : public Module {
private:
    bool  m_showBox        = true;
    bool  m_showHealthBar  = true;
    bool  m_showDistance   = true;
    bool  m_showName       = true;
    bool  m_showSnaplines  = false;
    int   m_boxStyle       = 0;      // 0 corners, 1 full
    float m_maxRange       = 64.0f;
    float m_lineThickness  = 1.5f;
    bool  m_interpolate    = true;

    float m_colorEnemy[4] = { 1.0f, 0.30f, 0.30f, 1.0f };

    std::vector<EntitySnapshot> m_targets;
    std::mutex m_mutex;

public:
    ESP() : Module("ESP", "Highlight players through walls", ModuleCategory::VISUAL, 'X')
    {
        Bind("Show Box", &m_showBox);
        Bind("Show Health Bar", &m_showHealthBar);
        Bind("Show Distance", &m_showDistance);
        Bind("Show Name", &m_showName);
        Bind("Show Snaplines", &m_showSnaplines);
        Bind("Box Style", &m_boxStyle);
        Bind("Max Range", &m_maxRange);
        Bind("Line Thickness", &m_lineThickness);
        Bind("Interpolate", &m_interpolate);
    }

    void OnDisable(JNIEnv*) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_targets.clear();
    }

    void OnTick(JNIEnv* env) override {
        if (!EntityList::Init(env)) return;

        auto ents = EntityList::GetPlayers(env, m_maxRange);
        auto snaps = EntityList::ToSnapshots(ents);

        std::lock_guard<std::mutex> lock(m_mutex);
        m_targets.swap(snaps);
    }

    // Render thread. Touches no JNI.
    void RenderESP() {
        if (!IsEnabled()) return;

        CameraView view = Camera::Get();
        if (!view.valid) return;

        std::vector<EntitySnapshot> targets;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_targets.empty()) return;
            targets = m_targets;
        }

        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.f || sh < 2.f) return;

        // Smooth between ticks so boxes do not stutter at 20 TPS
        // while the game renders at 200 FPS.
        float alpha = m_interpolate ? Camera::Alpha(view) : 1.0f;

        double camX, camY, camZ;
        view.EyeAt(alpha, &camX, &camY, &camZ);

        auto* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        const ImU32 outline = IM_COL32(0, 0, 0, 110);

        for (const auto& e : targets) {
            double ex = e.prevPosX + (e.posX - e.prevPosX) * alpha;
            double ey = e.prevPosY + (e.posY - e.prevPosY) * alpha;
            double ez = e.prevPosZ + (e.posZ - e.prevPosZ) * alpha;

            const double hw = 0.32;
            const double ht = 1.85;

            const double corners[8][3] = {
                { ex - hw, ey,      ez - hw }, { ex + hw, ey,      ez - hw },
                { ex - hw, ey,      ez + hw }, { ex + hw, ey,      ez + hw },
                { ex - hw, ey + ht, ez - hw }, { ex + hw, ey + ht, ez - hw },
                { ex - hw, ey + ht, ez + hw }, { ex + hw, ey + ht, ez + hw }
            };

            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            int visible = 0;

            for (int i = 0; i < 8; i++) {
                float x = 0.f, y = 0.f;
                if (!Camera::Project(view, corners[i][0], corners[i][1], corners[i][2],
                                     camX, camY, camZ, sw, sh, &x, &y)) continue;
                visible++;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }

            if (visible < 4) continue;

            float boxW = maxX - minX;
            float boxH = maxY - minY;
            if (boxW < 2.f || boxH < 2.f) continue;
            if (boxW > sw * 3.f || boxH > sh * 3.f) continue;
            if (maxX < 0.f || minX > sw || maxY < 0.f || minY > sh) continue;

            ImU32 col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(m_colorEnemy[0], m_colorEnemy[1],
                       m_colorEnemy[2], m_colorEnemy[3]));
            if (e.hurtTime > 0) col = IM_COL32(255, 255, 255, 225);

            if (m_showBox) {
                if (m_boxStyle == 0) {
                    float cl = (boxW < boxH ? boxW : boxH) * 0.25f;
                    if (cl < 4.f) cl = 4.f;
                    dl->AddLine(ImVec2(minX, minY), ImVec2(minX + cl, minY), col, m_lineThickness);
                    dl->AddLine(ImVec2(minX, minY), ImVec2(minX, minY + cl), col, m_lineThickness);
                    dl->AddLine(ImVec2(maxX, minY), ImVec2(maxX - cl, minY), col, m_lineThickness);
                    dl->AddLine(ImVec2(maxX, minY), ImVec2(maxX, minY + cl), col, m_lineThickness);
                    dl->AddLine(ImVec2(minX, maxY), ImVec2(minX + cl, maxY), col, m_lineThickness);
                    dl->AddLine(ImVec2(minX, maxY), ImVec2(minX, maxY - cl), col, m_lineThickness);
                    dl->AddLine(ImVec2(maxX, maxY), ImVec2(maxX - cl, maxY), col, m_lineThickness);
                    dl->AddLine(ImVec2(maxX, maxY), ImVec2(maxX, maxY - cl), col, m_lineThickness);
                } else {
                    dl->AddRect(ImVec2(minX, minY), ImVec2(maxX, maxY), col, 0, 0, m_lineThickness);
                }
                dl->AddRect(ImVec2(minX - 1, minY - 1), ImVec2(maxX + 1, maxY + 1),
                            outline, 0, 0, 1.0f);
            }

            if (m_showHealthBar) {
                float bw = 3.0f;
                float bx = minX - bw - 3.0f;
                float pct = e.health / e.maxHealth;
                if (pct < 0.f) pct = 0.f;
                if (pct > 1.f) pct = 1.f;
                float barTop = maxY - boxH * pct;

                dl->AddRectFilled(ImVec2(bx, minY), ImVec2(bx + bw, maxY),
                                  IM_COL32(0, 0, 0, 150));

                ImU32 hc = (pct > 0.5f)
                    ? IM_COL32((int)((1.f - (pct - 0.5f) * 2.f) * 255), 255, 0, 255)
                    : IM_COL32(255, (int)(pct * 2.f * 255), 0, 255);

                dl->AddRectFilled(ImVec2(bx, barTop), ImVec2(bx + bw, maxY), hc);
            }

            if (m_showName && !e.name.empty()) {
                ImVec2 ts = ImGui::CalcTextSize(e.name.c_str());
                float nx = minX + (boxW - ts.x) * 0.5f;
                float ny = minY - ts.y - 3.0f;
                dl->AddRectFilled(ImVec2(nx - 3, ny - 1),
                                  ImVec2(nx + ts.x + 3, ny + ts.y + 1),
                                  IM_COL32(0, 0, 0, 150), 3.0f);
                dl->AddText(ImVec2(nx, ny), IM_COL32(255, 255, 255, 255), e.name.c_str());
            }

            if (m_showDistance) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1fm", e.distanceToPlayer);
                ImVec2 ts = ImGui::CalcTextSize(buf);
                dl->AddText(ImVec2(minX + (boxW - ts.x) * 0.5f, maxY + 3.0f),
                            IM_COL32(205, 205, 205, 210), buf);
            }

            if (m_showSnaplines) {
                double t = e.distanceToPlayer / (double)m_maxRange;
                if (t > 1.0) t = 1.0;
                ImU32 lc = IM_COL32((int)(t * 255), (int)((1.0 - t) * 255), 0, 170);
                dl->AddLine(ImVec2(sw * 0.5f, sh),
                            ImVec2(minX + boxW * 0.5f, maxY), lc, 1.0f);
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
        ImGui::Checkbox("Interpolate", &m_interpolate);
        ImGui::SliderFloat("Max Range", &m_maxRange, 16.f, 128.f, "%.0f");
        ImGui::SliderFloat("Line Thickness", &m_lineThickness, 1.f, 4.f, "%.1f");
        ImGui::ColorEdit4("Color", m_colorEnemy);

        ImGui::Separator();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ImGui::TextDisabled("Tracking %d players", (int)m_targets.size());
        }
    }
};
