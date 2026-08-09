#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <mutex>
#include <chrono>
#include <algorithm>

// =================================================================
// ESP
// =================================================================
// WHY WE DO NOT READ GL MATRICES:
//
// The original version called glGetDoublev(GL_MODELVIEW_MATRIX) from
// the wglSwapBuffers hook. By that point Minecraft has already
// finished the world pass AND the GUI pass, so the matrix on the
// stack is the orthographic GUI matrix. Projecting world coordinates
// through it produces garbage.
//
// Instead we rebuild the camera transform ourselves from the values
// Minecraft used: interpolated eye position, yaw, pitch and FOV.
// That is deterministic and independent of when we run.
//
// THREADING:
// OnTick runs on the client thread and writes a plain-data snapshot
// under a mutex. Render runs on the game's render thread and only
// reads that snapshot, so it never touches JNI.
// =================================================================

class ESP : public Module {
private:
    // ---- Settings ----
    bool  m_showBox        = true;
    bool  m_showHealthBar  = true;
    bool  m_showDistance   = true;
    bool  m_showName       = true;
    bool  m_showSnaplines  = false;
    int   m_boxStyle       = 0;      // 0 = corners, 1 = full
    float m_maxRange       = 64.0f;
    float m_lineThickness  = 1.5f;
    bool  m_interpolate    = true;

    float m_colorEnemy[4] = { 1.0f, 0.30f, 0.30f, 1.0f };

    // ---- Snapshot shared with the render thread ----
    struct Frame {
        std::vector<EntitySnapshot> targets;
        double camX = 0, camY = 0, camZ = 0;
        double prevCamX = 0, prevCamY = 0, prevCamZ = 0;
        float  yaw = 0, pitch = 0;
        float  fov = 90.f;
        bool   valid = false;
        std::chrono::steady_clock::time_point stamp;
    };
    Frame m_frame;
    std::mutex m_mutex;

    jfieldID m_fFov = nullptr;
    bool m_fovResolved = false;

    // World point -> screen point using our own camera transform.
    // Returns false when the point is behind the camera.
    static bool Project(double wx, double wy, double wz,
                        double camX, double camY, double camZ,
                        float yawDeg, float pitchDeg, float fovDeg,
                        float screenW, float screenH,
                        float* outX, float* outY)
    {
        const double DEG = 3.14159265358979 / 180.0;

        double dx = wx - camX;
        double dy = wy - camY;
        double dz = wz - camZ;

        // Minecraft renders with rotate(pitch, X) then rotate(yaw+180, Y)
        double ya = (yawDeg + 180.0) * DEG;
        double cy = std::cos(ya), sy = std::sin(ya);
        double x1 =  dx * cy + dz * sy;
        double y1 =  dy;
        double z1 = -dx * sy + dz * cy;

        double pa = pitchDeg * DEG;
        double cp = std::cos(pa), sp = std::sin(pa);
        double x2 = x1;
        double y2 = y1 * cp - z1 * sp;
        double z2 = y1 * sp + z1 * cp;

        // Camera looks down -Z, so depth is -z2
        double depth = -z2;
        if (depth < 0.05) return false;

        double aspect = (screenH > 0) ? ((double)screenW / (double)screenH) : 1.777;
        double f = 1.0 / std::tan((fovDeg * 0.5) * DEG);

        double ndcX = (f / aspect) * (x2 / depth);
        double ndcY = f * (y2 / depth);

        *outX = (float)((ndcX * 0.5 + 0.5) * screenW);
        *outY = (float)((1.0 - (ndcY * 0.5 + 0.5)) * screenH);
        return true;
    }

public:
    ESP() : Module("ESP", "Highlight players through walls", ModuleCategory::VISUAL, 'X') {}

    void OnDisable(JNIEnv*) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frame.valid = false;
        m_frame.targets.clear();
    }

    void OnTick(JNIEnv* env) override {
        if (!EntityList::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_frame.valid = false;
            return;
        }

        if (!m_fovResolved) {
            if (ClassResolver::gameSettings) {
                m_fFov = JvmtiUtil::FindField(env, ClassResolver::gameSettings,
                    { "field_74334_X", "fovSetting" });
            }
            m_fovResolved = true;
        }

        float fov = 90.f;
        if (m_fFov) {
            jobject gs = Minecraft::GetGameSettings(env);
            if (gs) fov = env->GetFloatField(gs, m_fFov);
        }

        auto ents = EntityList::GetPlayers(env, m_maxRange);

        Frame f;
        f.targets  = EntityList::ToSnapshots(ents);
        f.camX     = Minecraft::GetPosX(env, player);
        f.camY     = Minecraft::GetPosY(env, player) + 1.62;
        f.camZ     = Minecraft::GetPosZ(env, player);
        f.prevCamX = Minecraft::GetPrevPosX(env, player);
        f.prevCamY = Minecraft::GetPrevPosY(env, player) + 1.62;
        f.prevCamZ = Minecraft::GetPrevPosZ(env, player);
        f.yaw      = Minecraft::GetYaw(env, player);
        f.pitch    = Minecraft::GetPitch(env, player);
        f.fov      = fov;
        f.valid    = true;
        f.stamp    = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_frame = std::move(f);
        }
    }

    // Called from the render thread during the ImGui frame.
    // Touches no JNI.
    void RenderESP() {
        if (!m_enabled) return;

        Frame f;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_frame.valid || m_frame.targets.empty()) return;
            f = m_frame;
        }

        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.f || sh < 2.f) return;

        // Interpolate between ticks so ESP does not stutter at 20 TPS
        // while the game renders at 200 FPS.
        float alpha = 1.0f;
        if (m_interpolate) {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - f.stamp).count();
            alpha = (float)ms / 50.0f;
            if (alpha < 0.f) alpha = 0.f;
            if (alpha > 1.f) alpha = 1.f;
        }

        double camX = f.prevCamX + (f.camX - f.prevCamX) * alpha;
        double camY = f.prevCamY + (f.camY - f.prevCamY) * alpha;
        double camZ = f.prevCamZ + (f.camZ - f.prevCamZ) * alpha;

        auto* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        const ImU32 outline = IM_COL32(0, 0, 0, 110);

        for (const auto& e : f.targets) {
            double ex = e.prevPosX + (e.posX - e.prevPosX) * alpha;
            double ey = e.prevPosY + (e.posY - e.prevPosY) * alpha;
            double ez = e.prevPosZ + (e.posZ - e.prevPosZ) * alpha;

            const double hw = 0.32;   // player half-width
            const double ht = 1.85;   // player height

            const double corners[8][3] = {
                { ex - hw, ey,      ez - hw }, { ex + hw, ey,      ez - hw },
                { ex - hw, ey,      ez + hw }, { ex + hw, ey,      ez + hw },
                { ex - hw, ey + ht, ez - hw }, { ex + hw, ey + ht, ez - hw },
                { ex - hw, ey + ht, ez + hw }, { ex + hw, ey + ht, ez + hw }
            };

            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            int visible = 0;

            for (int i = 0; i < 8; i++) {
                float sx = 0.f, sy = 0.f;
                if (!Project(corners[i][0], corners[i][1], corners[i][2],
                             camX, camY, camZ, f.yaw, f.pitch, f.fov,
                             sw, sh, &sx, &sy)) continue;
                visible++;
                if (sx < minX) minX = sx;
                if (sx > maxX) maxX = sx;
                if (sy < minY) minY = sy;
                if (sy > maxY) maxY = sy;
            }

            if (visible < 4) continue;   // mostly behind the camera

            float boxW = maxX - minX;
            float boxH = maxY - minY;
            if (boxW < 2.f || boxH < 2.f) continue;
            if (boxW > sw * 3.f || boxH > sh * 3.f) continue;
            if (maxX < 0.f || minX > sw || maxY < 0.f || minY > sh) continue;

            ImU32 col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(m_colorEnemy[0], m_colorEnemy[1], m_colorEnemy[2], m_colorEnemy[3]));
            if (e.hurtTime > 0) col = IM_COL32(255, 255, 255, 225);

            // ---- Box ----
            if (m_showBox) {
                if (m_boxStyle == 0) {
                    float cl = boxW < boxH ? boxW : boxH;
                    cl *= 0.25f;
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

            // ---- Health bar ----
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

            // ---- Name ----
            if (m_showName && !e.name.empty()) {
                ImVec2 ts = ImGui::CalcTextSize(e.name.c_str());
                float nx = minX + (boxW - ts.x) * 0.5f;
                float ny = minY - ts.y - 3.0f;
                dl->AddRectFilled(ImVec2(nx - 3, ny - 1),
                                  ImVec2(nx + ts.x + 3, ny + ts.y + 1),
                                  IM_COL32(0, 0, 0, 150), 3.0f);
                dl->AddText(ImVec2(nx, ny), IM_COL32(255, 255, 255, 255), e.name.c_str());
            }

            // ---- Distance ----
            if (m_showDistance) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1fm", e.distanceToPlayer);
                ImVec2 ts = ImGui::CalcTextSize(buf);
                dl->AddText(ImVec2(minX + (boxW - ts.x) * 0.5f, maxY + 3.0f),
                            IM_COL32(205, 205, 205, 210), buf);
            }

            // ---- Snapline ----
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
            ImGui::TextDisabled("Tracking %d players | fov %.0f",
                (int)m_frame.targets.size(), m_frame.fov);
        }
    }
};
