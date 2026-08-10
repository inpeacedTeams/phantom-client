#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../render/camera.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>

// =================================================================
// ESP
// =================================================================
// WHAT WAS WRONG WITH THE OLD ONE
//
// 1. IT VANISHED AT CLOSE RANGE AND ODD ANGLES.
//    It projected eight corners, dropped any behind the camera and
//    gave up below four. Standing next to someone puts their lower
//    corners behind the near plane while their head is still in
//    view, so the box collapsed or disappeared. Camera::ProjectBox
//    now clips the twelve edges against the near plane instead.
//
// 2. IT FLICKERED.
//    Every frame drew whatever the last tick happened to contain.
//    One missed scan, one entity dropping out of range for a single
//    tick, and the box blinked. Entities are now TRACKED by id and
//    carry their own alpha, so appearing and vanishing are fades
//    and a one-tick gap is invisible.
//
// 3. IT COPIED THE WHOLE TARGET LIST EVERY FRAME.
//    Including a std::string per player, at 300 FPS, under a lock,
//    for data that only changes 20 times a second. The snapshot now
//    carries a version and the render thread copies only when that
//    version moves.
//
// 4. THE BOX WAS A GUESS.
//    Hard-coded 0.32 by 1.85. Sneaking players sat below their box
//    and the box never matched the hitbox you were actually aiming
//    at. Real width and height come from the entity now.
//
// THREADING
// OnTick runs on the client thread and publishes plain data under a
// mutex. RenderESP runs on the render thread and never touches JNI.
// =================================================================

class ESP : public Module {
private:
    // ---- Core ----
    // 0 Corners, 1 Full box, 2 Outline only
    int   m_boxStyle   = 0;
    bool  m_showBox    = true;
    bool  m_showHealth = true;
    bool  m_showName   = true;
    bool  m_showDistance = false;
    float m_maxRange   = 64.0f;

    // ---- Advanced ----
    bool  m_showTracers   = false;
    int   m_tracerOrigin  = 0;      // 0 bottom, 1 crosshair
    bool  m_healthColor   = false;  // tint the box by health
    bool  m_distanceFade  = true;
    float m_lineThickness = 1.4f;
    bool  m_interpolate   = true;
    bool  m_smoothBoxes   = true;
    float m_fadeSpeed     = 12.0f;
    bool  m_shadowText    = true;

    float m_color[4] = { 0.36f, 0.72f, 1.00f, 1.00f };

    // ---- Published by the client thread ----
    std::vector<EntitySnapshot> m_shared;
    std::mutex m_mutex;
    std::atomic<unsigned> m_version{ 0 };
    std::atomic<int> m_count{ 0 };

    // ---- Render thread only ----
    struct Track {
        EntitySnapshot snap;
        float alpha = 0.0f;      // 0 gone, 1 fully drawn
        bool  seen = false;      // present in the newest snapshot
        bool  hasBox = false;
        ScreenBox box;           // smoothed screen bounds
        int   lastFrame = 0;
    };

    std::vector<EntitySnapshot> m_local;   // render-thread copy
    unsigned m_localVersion = 0;
    std::unordered_map<int, Track> m_tracks;

    mutable char m_status[32] = {};

    static float Clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // Frame-rate independent easing. A plain per-frame lerp runs at
    // a different speed on every machine.
    static float Approach(float cur, float target, float speed, float dt) {
        return cur + (target - cur) * (1.0f - std::exp(-speed * dt));
    }

    static ImU32 WithAlpha(const float rgba[4], float a) {
        return ImGui::ColorConvertFloat4ToU32(
            ImVec4(rgba[0], rgba[1], rgba[2], rgba[3] * a));
    }

    // Green through yellow to red
    static ImU32 HealthColor(float pct, float a) {
        pct = Clamp01(pct);
        float r = pct > 0.5f ? (1.0f - pct) * 2.0f : 1.0f;
        float g = pct > 0.5f ? 1.0f : pct * 2.0f;
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, 0.15f, a));
    }

    void DrawText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text,
                  float a) const
    {
        if (m_shadowText) {
            dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                        IM_COL32(0, 0, 0, (int)(170 * a)), text);
        }
        dl->AddText(at, col, text);
    }

    // Corner brackets. Cleaner than a full rectangle at a glance and
    // it stays legible against a busy world.
    void DrawCorners(ImDrawList* dl, const ScreenBox& b, ImU32 col,
                     float thickness) const
    {
        float w = b.Width(), h = b.Height();
        float len = (w < h ? w : h) * 0.28f;
        if (len < 3.0f) len = 3.0f;
        if (len > 14.0f) len = 14.0f;

        dl->AddLine(ImVec2(b.minX, b.minY), ImVec2(b.minX + len, b.minY), col, thickness);
        dl->AddLine(ImVec2(b.minX, b.minY), ImVec2(b.minX, b.minY + len), col, thickness);
        dl->AddLine(ImVec2(b.maxX, b.minY), ImVec2(b.maxX - len, b.minY), col, thickness);
        dl->AddLine(ImVec2(b.maxX, b.minY), ImVec2(b.maxX, b.minY + len), col, thickness);
        dl->AddLine(ImVec2(b.minX, b.maxY), ImVec2(b.minX + len, b.maxY), col, thickness);
        dl->AddLine(ImVec2(b.minX, b.maxY), ImVec2(b.minX, b.maxY - len), col, thickness);
        dl->AddLine(ImVec2(b.maxX, b.maxY), ImVec2(b.maxX - len, b.maxY), col, thickness);
        dl->AddLine(ImVec2(b.maxX, b.maxY), ImVec2(b.maxX, b.maxY - len), col, thickness);
    }

public:
    ESP() : Module("ESP", "Highlight players through walls",
                   ModuleCategory::VISUAL, 'X')
    {
        Bind("Box Style", &m_boxStyle);
        Bind("Show Box", &m_showBox);
        Bind("Show Health", &m_showHealth);
        Bind("Show Name", &m_showName);
        Bind("Show Distance", &m_showDistance);
        Bind("Max Range", &m_maxRange);
        Bind("Show Tracers", &m_showTracers);
        Bind("Tracer Origin", &m_tracerOrigin);
        Bind("Health Color", &m_healthColor);
        Bind("Distance Fade", &m_distanceFade);
        Bind("Line Thickness", &m_lineThickness);
        Bind("Interpolate", &m_interpolate);
        Bind("Smooth Boxes", &m_smoothBoxes);
        Bind("Fade Speed", &m_fadeSpeed);
        Bind("Shadow Text", &m_shadowText);
    }

    void OnDisable(JNIEnv*) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shared.clear();
        }
        m_version.fetch_add(1);
        m_count.store(0);
        m_status[0] = '\0';
        // Tracks are owned by the render thread and fade themselves
        // out on the next frame, so the boxes dissolve rather than
        // snapping off the instant the switch moves.
    }

    // ---- Client thread ----
    void OnTick(JNIEnv* env) override {
        if (!EntityList::Init(env)) return;

        auto ents  = EntityList::GetPlayers(env, m_maxRange);
        auto snaps = EntityList::ToSnapshots(ents);
        int n = (int)snaps.size();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shared.swap(snaps);
        }
        m_version.fetch_add(1, std::memory_order_release);
        m_count.store(n, std::memory_order_relaxed);

        snprintf(m_status, sizeof(m_status), "%d visible", n);
    }

    const char* StatusLine() const override {
        return m_status[0] ? m_status : nullptr;
    }

    // =============================================================
    // Render thread. No JNI.
    // =============================================================
    void RenderESP() {
        CameraView view = Camera::Get();
        ImGuiIO& io = ImGui::GetIO();

        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.f || sh < 2.f) return;

        float dt = io.DeltaTime;
        if (dt > 0.1f) dt = 0.1f;
        if (dt <= 0.0f) dt = 0.016f;

        // ---- Refresh the local copy only when the tick moved ----
        unsigned v = m_version.load(std::memory_order_acquire);
        if (v != m_localVersion) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_local = m_shared;
            m_localVersion = v;
        }

        bool on = IsEnabled() && view.valid;

        // ---- Fold the snapshot into the tracks ----
        for (auto& kv : m_tracks) kv.second.seen = false;

        if (on) {
            for (const auto& s : m_local) {
                if (s.id < 0) continue;
                Track& t = m_tracks[s.id];
                t.snap = s;
                t.seen = true;
            }
        }

        int frame = ImGui::GetFrameCount();
        auto* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        float alpha = m_interpolate ? Camera::Alpha(view) : 1.0f;
        double camX = 0, camY = 0, camZ = 0;
        if (view.valid) view.EyeAt(alpha, &camX, &camY, &camZ);

        for (auto it = m_tracks.begin(); it != m_tracks.end(); ) {
            Track& t = it->second;

            float target = t.seen ? 1.0f : 0.0f;
            t.alpha = Approach(t.alpha, target, m_fadeSpeed, dt);

            // Fully faded and gone: stop paying for it
            if (!t.seen && t.alpha < 0.01f) {
                it = m_tracks.erase(it);
                continue;
            }
            t.lastFrame = frame;

            if (view.valid && t.alpha > 0.01f)
                DrawTrack(dl, view, t, camX, camY, camZ, alpha, sw, sh, dt);

            ++it;
        }
    }

private:
    void DrawTrack(ImDrawList* dl, const CameraView& view, Track& t,
                   double camX, double camY, double camZ,
                   float lerp, float sw, float sh, float dt)
    {
        const EntitySnapshot& e = t.snap;

        // Position between the last two ticks, so a 20 TPS entity
        // moves smoothly at any frame rate.
        double ex = e.prevPosX + (e.posX - e.prevPosX) * lerp;
        double ey = e.prevPosY + (e.posY - e.prevPosY) * lerp;
        double ez = e.prevPosZ + (e.posZ - e.prevPosZ) * lerp;

        double hw = e.width * 0.5 + 0.06;
        double ht = e.height + 0.12;

        ScreenBox box = Camera::ProjectBox(view,
            ex - hw, ey, ez - hw,
            ex + hw, ey + ht, ez + hw,
            camX, camY, camZ, sw, sh);

        if (!box.valid || !Camera::OnScreen(box, sw, sh)) {
            t.hasBox = false;
            return;
        }

        // A box wider than a few screens means the camera is inside
        // the hitbox. Drawing it is meaningless and expensive.
        if (box.Width() > sw * 4.0f || box.Height() > sh * 4.0f) {
            t.hasBox = false;
            return;
        }

        // Light smoothing removes the sub-pixel crawl that comes
        // from double to float rounding. Fast enough that it never
        // trails a moving player.
        if (m_smoothBoxes && t.hasBox) {
            const float k = 26.0f;
            box.minX = Approach(t.box.minX, box.minX, k, dt);
            box.minY = Approach(t.box.minY, box.minY, k, dt);
            box.maxX = Approach(t.box.maxX, box.maxX, k, dt);
            box.maxY = Approach(t.box.maxY, box.maxY, k, dt);
        }
        t.box = box;
        t.hasBox = true;

        // ---- Opacity ----
        float a = t.alpha;
        if (m_distanceFade && m_maxRange > 1.0f) {
            float d = (float)(e.distanceToPlayer / m_maxRange);
            a *= 1.0f - 0.55f * Clamp01(d);
        }
        if (a < 0.02f) return;

        float hpPct = (e.maxHealth > 0.f) ? (e.health / e.maxHealth) : 1.0f;

        ImU32 col = m_healthColor ? HealthColor(hpPct, m_color[3] * a)
                                  : WithAlpha(m_color, a);
        // Flash white on the tick they take damage: the clearest
        // possible "your hit landed" feedback.
        if (e.hurtTime > 0) col = IM_COL32(255, 255, 255, (int)(235 * a));

        ImU32 shadow = IM_COL32(0, 0, 0, (int)(120 * a));

        // ---- Box ----
        if (m_showBox) {
            float th = m_lineThickness;
            switch (m_boxStyle) {
                case 1:
                    dl->AddRect(ImVec2(box.minX - 1, box.minY - 1),
                                ImVec2(box.maxX + 1, box.maxY + 1),
                                shadow, 0, 0, th + 1.5f);
                    dl->AddRect(ImVec2(box.minX, box.minY),
                                ImVec2(box.maxX, box.maxY), col, 0, 0, th);
                    break;
                case 2:
                    dl->AddRect(ImVec2(box.minX, box.minY),
                                ImVec2(box.maxX, box.maxY), col, 2.0f, 0, th);
                    break;
                default:
                    DrawCorners(dl, box, shadow, th + 1.6f);
                    DrawCorners(dl, box, col, th);
                    break;
            }
        }

        // ---- Health bar ----
        if (m_showHealth) {
            float bw = 2.5f;
            float bx = box.minX - bw - 3.0f;
            float top = box.maxY - box.Height() * Clamp01(hpPct);

            dl->AddRectFilled(ImVec2(bx - 1, box.minY - 1),
                              ImVec2(bx + bw + 1, box.maxY + 1),
                              IM_COL32(0, 0, 0, (int)(150 * a)), 1.5f);
            dl->AddRectFilled(ImVec2(bx, top), ImVec2(bx + bw, box.maxY),
                              HealthColor(hpPct, a), 1.5f);
        }

        // ---- Name ----
        if (m_showName && !e.name.empty()) {
            const char* txt = e.name.c_str();
            ImVec2 ts = ImGui::CalcTextSize(txt);
            float nx = box.CenterX() - ts.x * 0.5f;
            float ny = box.minY - ts.y - 4.0f;

            dl->AddRectFilled(ImVec2(nx - 4, ny - 2),
                              ImVec2(nx + ts.x + 4, ny + ts.y + 2),
                              IM_COL32(0, 0, 0, (int)(130 * a)), 3.0f);
            DrawText(dl, ImVec2(nx, ny),
                     IM_COL32(255, 255, 255, (int)(255 * a)), txt, a);
        }

        // ---- Distance ----
        if (m_showDistance) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%.0fm", e.distanceToPlayer);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            DrawText(dl, ImVec2(box.CenterX() - ts.x * 0.5f, box.maxY + 3.0f),
                     IM_COL32(215, 215, 215, (int)(220 * a)), buf, a);
        }

        // ---- Tracer ----
        if (m_showTracers) {
            ImVec2 from = (m_tracerOrigin == 1)
                        ? ImVec2(sw * 0.5f, sh * 0.5f)
                        : ImVec2(sw * 0.5f, sh);
            ImVec2 to(box.CenterX(), box.maxY);
            dl->AddLine(from, to, WithAlpha(m_color, a * 0.55f), 1.0f);
        }
    }

public:
    // =============================================================
    // Panel
    // =============================================================
    void RenderSettings() override {
        const char* styles[] = { "Corners", "Full Box", "Rounded" };
        ImGui::Combo("Box Style", &m_boxStyle, styles, 3);

        ImGui::Checkbox("Box", &m_showBox);
        ImGui::SameLine();
        ImGui::Checkbox("Health", &m_showHealth);
        ImGui::Checkbox("Name", &m_showName);
        ImGui::SameLine();
        ImGui::Checkbox("Distance", &m_showDistance);

        ImGui::SliderFloat("Range", &m_maxRange, 16.f, 128.f, "%.0f m");

        ImGui::ColorEdit4("Colour", m_color,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_AlphaBar);

        if (!Camera::Get().valid) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Camera not resolved yet: join a world");
        }
    }

    bool HasAdvanced() const override { return true; }

    void RenderAdvanced() override {
        ImGui::TextDisabled("Tracking %d, drawing %d",
            m_count.load(), (int)m_tracks.size());

        ImGui::SeparatorText("Extras");
        ImGui::Checkbox("Tracers", &m_showTracers);
        if (m_showTracers) {
            const char* origins[] = { "Screen bottom", "Crosshair" };
            ImGui::Combo("Tracer From", &m_tracerOrigin, origins, 2);
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Very obvious on a recording");
        }
        ImGui::Checkbox("Colour By Health", &m_healthColor);
        ImGui::Checkbox("Fade With Distance", &m_distanceFade);
        ImGui::Checkbox("Text Shadow", &m_shadowText);

        ImGui::SeparatorText("Rendering");
        ImGui::SliderFloat("Line Thickness", &m_lineThickness, 1.f, 4.f, "%.1f");
        ImGui::Checkbox("Interpolate", &m_interpolate);
        ImGui::TextDisabled("Smooths motion between the 20 position updates "
                            "a second the server actually sends.");
        ImGui::Checkbox("Smooth Boxes", &m_smoothBoxes);
        ImGui::TextDisabled("Removes sub-pixel crawl on stationary targets.");
        ImGui::SliderFloat("Fade Speed", &m_fadeSpeed, 4.f, 30.f, "%.0f");
        ImGui::TextDisabled("Lower is a softer appear and disappear. This is "
                            "what stops a one-tick gap looking like a flicker.");
    }
};
