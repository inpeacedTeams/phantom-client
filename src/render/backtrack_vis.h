#pragma once
#include <imgui.h>
#include <cstdio>
#include <cmath>
#include <vector>

#include "camera.h"
#include "../modules/combat/backtrack.h"

// =================================================================
// BacktrackVis
// =================================================================
// Draws where Backtrack is holding each target, so you can see how
// far behind their real position you are actually swinging.
//
// Runs on the render thread. Reads only the plain-data snapshot the
// module publishes, so no JNI here.
//
// STYLES
//   0 Box        filled quad at the held position
//   1 Wireframe  full hitbox outline, all twelve edges
//   2 Trail      the recorded path from held spot to real one
//   3 Marker     small cross, the least intrusive option
//   4 Text Only  numbers, no geometry
// =================================================================

class BacktrackVis {
private:
    static constexpr double kHalfWidth = 0.32;
    static constexpr double kHeight    = 1.85;

    struct Box2D {
        float minX, minY, maxX, maxY;
        bool  ok = false;
    };

    // Project the eight corners of a player hitbox and take the
    // screen-space bounds.
    static Box2D ProjectHitbox(const CameraView& v,
                               double ex, double ey, double ez,
                               double camX, double camY, double camZ,
                               float sw, float sh)
    {
        Box2D b;
        const double hw = kHalfWidth, ht = kHeight;
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
            if (!Camera::Project(v, corners[i][0], corners[i][1], corners[i][2],
                                 camX, camY, camZ, sw, sh, &x, &y)) continue;
            visible++;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }

        if (visible < 4) return b;

        float w = maxX - minX, h = maxY - minY;
        if (w < 2.f || h < 2.f) return b;
        if (w > sw * 3.f || h > sh * 3.f) return b;
        if (maxX < 0.f || minX > sw || maxY < 0.f || minY > sh) return b;

        b.minX = minX; b.minY = minY; b.maxX = maxX; b.maxY = maxY;
        b.ok = true;
        return b;
    }

    // Twelve edges of the box, drawn in 3D rather than as a 2D rect.
    // Reads correctly at an angle, where a flat rectangle does not.
    static void DrawWireframe(ImDrawList* dl, const CameraView& v,
                              double ex, double ey, double ez,
                              double camX, double camY, double camZ,
                              float sw, float sh, ImU32 col, float thick)
    {
        const double hw = kHalfWidth, ht = kHeight;
        const double c[8][3] = {
            { ex - hw, ey,      ez - hw }, { ex + hw, ey,      ez - hw },
            { ex + hw, ey,      ez + hw }, { ex - hw, ey,      ez + hw },
            { ex - hw, ey + ht, ez - hw }, { ex + hw, ey + ht, ez - hw },
            { ex + hw, ey + ht, ez + hw }, { ex - hw, ey + ht, ez + hw }
        };

        ImVec2 pts[8];
        bool   ok[8];
        for (int i = 0; i < 8; i++) {
            float x = 0.f, y = 0.f;
            ok[i] = Camera::Project(v, c[i][0], c[i][1], c[i][2],
                                    camX, camY, camZ, sw, sh, &x, &y);
            pts[i] = ImVec2(x, y);
        }

        static const int edges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},        // bottom
            {4,5},{5,6},{6,7},{7,4},        // top
            {0,4},{1,5},{2,6},{3,7}         // uprights
        };

        for (auto& e : edges) {
            if (!ok[e[0]] || !ok[e[1]]) continue;
            dl->AddLine(pts[e[0]], pts[e[1]], col, thick);
        }
    }

public:
    static void Render(Backtrack* bt) {
        if (!bt || !bt->VisEnabled()) return;

        CameraView view = Camera::Get();
        if (!view.valid) return;

        auto targets = bt->TakeVis();
        if (targets.empty()) return;

        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.f || sh < 2.f) return;

        auto* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        float alpha = Camera::Alpha(view);
        double camX, camY, camZ;
        view.EyeAt(alpha, &camX, &camY, &camZ);

        const float* c = bt->VisColor();
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(c[0], c[1], c[2], c[3]));
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
            ImVec4(c[0], c[1], c[2], c[3] * 0.18f));
        const ImU32 ghost = ImGui::ColorConvertFloat4ToU32(
            ImVec4(c[0], c[1], c[2], bt->VisGhostAlpha()));

        const int   style = bt->VisStyle();
        const float thick = bt->VisThickness();

        for (const auto& t : targets) {
            if (!t.rewound) continue;

            Box2D held = ProjectHitbox(view, t.backX, t.backY, t.backZ,
                                       camX, camY, camZ, sw, sh);

            // ---- Ghost at the real position ----
            // Seeing both at once is the point: the gap between them
            // is exactly what you are exploiting.
            Box2D real;
            if (bt->VisGhost()) {
                real = ProjectHitbox(view, t.trueX, t.trueY, t.trueZ,
                                     camX, camY, camZ, sw, sh);
                if (real.ok) {
                    dl->AddRect(ImVec2(real.minX, real.minY),
                                ImVec2(real.maxX, real.maxY),
                                ghost, 0, 0, 1.0f);
                }
            }

            // ---- Link line ----
            if (bt->VisLink() && held.ok && real.ok) {
                ImVec2 a((held.minX + held.maxX) * 0.5f, held.maxY);
                ImVec2 b((real.minX + real.maxX) * 0.5f, real.maxY);
                dl->AddLine(a, b, ghost, 1.0f);
            }

            // ---- Held position ----
            switch (style) {
                case 0: {   // Box
                    if (!held.ok) break;
                    dl->AddRectFilled(ImVec2(held.minX, held.minY),
                                      ImVec2(held.maxX, held.maxY), fill);
                    dl->AddRect(ImVec2(held.minX, held.minY),
                                ImVec2(held.maxX, held.maxY), col, 0, 0, thick);
                    break;
                }
                case 1: {   // Wireframe
                    DrawWireframe(dl, view, t.backX, t.backY, t.backZ,
                                  camX, camY, camZ, sw, sh, col, thick);
                    break;
                }
                case 2: {   // Trail
                    ImVec2 prev;
                    bool havePrev = false;
                    int n = (int)t.trail.size();
                    for (int i = 0; i < n; i++) {
                        float x = 0.f, y = 0.f;
                        if (!Camera::Project(view, t.trail[i][0],
                                             t.trail[i][1] + kHeight * 0.5,
                                             t.trail[i][2],
                                             camX, camY, camZ, sw, sh, &x, &y)) {
                            havePrev = false;
                            continue;
                        }
                        ImVec2 p(x, y);
                        if (havePrev) {
                            // Older samples fade out, so the direction
                            // of travel reads at a glance.
                            float f = (float)i / (float)(n > 1 ? n - 1 : 1);
                            ImU32 seg = ImGui::ColorConvertFloat4ToU32(
                                ImVec4(c[0], c[1], c[2], c[3] * (0.15f + 0.85f * f)));
                            dl->AddLine(prev, p, seg, thick);
                        }
                        prev = p;
                        havePrev = true;
                    }
                    if (held.ok) {
                        dl->AddRect(ImVec2(held.minX, held.minY),
                                    ImVec2(held.maxX, held.maxY), col, 0, 0, thick);
                    }
                    break;
                }
                case 3: {   // Marker
                    float x = 0.f, y = 0.f;
                    if (!Camera::Project(view, t.backX, t.backY + kHeight * 0.5,
                                         t.backZ, camX, camY, camZ,
                                         sw, sh, &x, &y)) break;
                    float s = 6.0f;
                    dl->AddLine(ImVec2(x - s, y), ImVec2(x + s, y), col, thick);
                    dl->AddLine(ImVec2(x, y - s), ImVec2(x, y + s), col, thick);
                    dl->AddCircle(ImVec2(x, y), s * 1.6f, col, 0, thick * 0.8f);
                    break;
                }
                default:
                    break;   // Text only
            }

            // ---- Readout ----
            if (!bt->VisShowMs() && !bt->VisShowDist()) continue;

            char buf[64];
            if (bt->VisShowMs() && bt->VisShowDist())
                snprintf(buf, sizeof(buf), "%d ms  %.2f m", t.delayMs, t.offset);
            else if (bt->VisShowMs())
                snprintf(buf, sizeof(buf), "%d ms", t.delayMs);
            else
                snprintf(buf, sizeof(buf), "%.2f m", t.offset);

            float tx, ty;
            if (held.ok) {
                ImVec2 ts = ImGui::CalcTextSize(buf);
                tx = held.minX + (held.maxX - held.minX - ts.x) * 0.5f;
                ty = held.minY - ts.y - 4.0f;
            } else {
                float x = 0.f, y = 0.f;
                if (!Camera::Project(view, t.backX, t.backY + kHeight,
                                     t.backZ, camX, camY, camZ,
                                     sw, sh, &x, &y)) continue;
                ImVec2 ts = ImGui::CalcTextSize(buf);
                tx = x - ts.x * 0.5f;
                ty = y - ts.y - 4.0f;
            }

            ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddRectFilled(ImVec2(tx - 3, ty - 1),
                              ImVec2(tx + ts.x + 3, ty + ts.y + 1),
                              IM_COL32(0, 0, 0, 150), 3.0f);
            dl->AddText(ImVec2(tx, ty), col, buf);
        }
    }
};
