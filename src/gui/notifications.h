#pragma once
#include <imgui.h>
#include <string>
#include <deque>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdarg>

#include "ios_theme.h"

// =================================================================
// Notifications
// =================================================================
// The client had exactly one way to tell you something: printf to
// an AllocConsole window sitting behind a fullscreen game. Which is
// to say it had none. Every "keybinds unresolved", every failed
// config write, every module that quietly disabled itself, went
// somewhere nobody would ever look.
//
// These are the same cards as the HUD, in the same language as the
// rest of the client, stacked bottom-right and self-dismissing.
//
// WHAT GETS A NOTIFICATION
//   Something the user did          config saved, config loaded
//   Something that failed           could not write, could not bind
//   Something that changed itself   module disabled after an error
//
// What does NOT: anything routine. A toast for every module toggle
// would be noise, and the switch already animated.
//
// THREADING
// Push() may be called from any thread, including the client tick
// and the click timer, so the queue is behind a mutex. Render()
// runs on the render thread only.
//
// LIFETIME
// Each entry owns its own clock, so a stall does not dump five
// notifications at once, and the stack animates: a dismissed card
// slides out and the ones below rise into its place rather than
// snapping up.
// =================================================================

namespace iOS {

class Notify {
public:
    enum class Kind { Info, Success, Warning, Error };

private:
    struct Item {
        std::string title;
        std::string body;
        Kind  kind = Kind::Info;
        float life = 0.0f;      // seconds remaining
        float total = 4.0f;
        float anim = 0.0f;      // 0 offscreen, 1 in place
        float y = 0.0f;         // animated slot, -1 means unplaced
        bool  placed = false;
    };

    inline static std::deque<Item> s_items;
    inline static std::mutex s_mutex;

    static constexpr size_t kMax = 5;

    static ImU32 KindColor(Kind k) {
        switch (k) {
            case Kind::Success: return Col::Green;
            case Kind::Warning: return Col::Orange;
            case Kind::Error:   return Col::Red;
            default:            return Col::Blue;
        }
    }

    // Thin names over the shared curves in iOS::Ease, so there is
    // one definition of each curve in the client.
    static float Clamp01(float v) { return iOS::Ease::Clamp01(v); }
    static float EaseOut(float t) { return iOS::Ease::Out(t); }

public:
    // ---- Any thread ----
    static void Push(Kind kind, const std::string& title,
                     const std::string& body = "", float seconds = 4.0f)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        // Repeating the same message is almost always a loop
        // shouting once per tick. Refresh the existing card instead
        // of stacking forty copies.
        for (auto& it : s_items) {
            if (it.title == title && it.body == body) {
                it.life = seconds;
                it.total = seconds;
                return;
            }
        }

        Item n;
        n.title = title;
        n.body  = body;
        n.kind  = kind;
        n.life  = seconds;
        n.total = seconds;
        s_items.push_back(std::move(n));

        while (s_items.size() > kMax) s_items.pop_front();
    }

    static void Info(const std::string& t, const std::string& b = "") {
        Push(Kind::Info, t, b);
    }
    static void Success(const std::string& t, const std::string& b = "") {
        Push(Kind::Success, t, b, 3.0f);
    }
    static void Warning(const std::string& t, const std::string& b = "") {
        Push(Kind::Warning, t, b, 5.0f);
    }
    static void Error(const std::string& t, const std::string& b = "") {
        Push(Kind::Error, t, b, 6.5f);
    }

    static void Clear() {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_items.clear();
    }

    // ---- Render thread ----
    static void Render() {
        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x, sh = io.DisplaySize.y;
        if (sw < 2.0f || sh < 2.0f) return;

        float dt = io.DeltaTime;
        if (dt > 0.1f) dt = 0.1f;
        if (dt <= 0.0f) dt = 0.016f;

        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_items.empty()) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        const float margin = 16.0f;
        const float width  = 296.0f;
        const float padX   = 14.0f;
        const float padY   = 11.0f;
        const float gap    = 8.0f;

        // Bottom up, newest nearest the corner.
        float slot = sh - margin;

        for (int i = (int)s_items.size() - 1; i >= 0; i--) {
            Item& n = s_items[(size_t)i];

            n.life -= dt;

            // Fade in over the first fifth of a second, out over the
            // last third of one.
            float in  = Clamp01((n.total - n.life) / 0.22f);
            float out = Clamp01(n.life / 0.34f);
            float target = (n.life > 0.0f) ? (in < out ? in : out) : 0.0f;
            n.anim += (target - n.anim) * (1.0f - std::exp(-20.0f * dt));

            // ---- Measure ----
            Fonts::Push(Fonts::BodyBold);
            ImVec2 ts = ImGui::CalcTextSize(n.title.c_str());
            Fonts::Pop(Fonts::BodyBold);

            float wrap = width - padX * 2.0f - 10.0f;
            ImVec2 bs(0, 0);
            if (!n.body.empty()) {
                Fonts::Push(Fonts::Caption);
                bs = ImGui::CalcTextSize(n.body.c_str(), nullptr, false, wrap);
                Fonts::Pop(Fonts::Caption);
            }

            float h = padY * 2.0f + ts.y + (bs.y > 0.0f ? bs.y + 4.0f : 0.0f);

            float targetY = slot - h;
            if (!n.placed) { n.y = targetY; n.placed = true; }
            n.y += (targetY - n.y) * (1.0f - std::exp(-16.0f * dt));

            slot = n.y - gap;

            float a = EaseOut(n.anim);
            if (a > 0.004f) {
                // Slides in from the right edge as it fades
                float x = sw - margin - width + (1.0f - a) * 24.0f;
                ImVec2 p0(x, n.y);
                ImVec2 p1(x + width, n.y + h);

                ImU32 accent = KindColor(n.kind);

                dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 3.0f),
                                  ImVec2(p1.x + 1.0f, p1.y + 4.0f),
                                  IM_COL32(0, 0, 0, (int)(40 * a)),
                                  M::CardRadius);

                dl->AddRectFilled(p0, p1,
                                  IM_COL32(252, 252, 254, (int)(240 * a)),
                                  M::CardRadius);

                // Accent stripe down the left, the only colour on
                // the card, which is what makes severity readable at
                // a glance without reading the text.
                dl->AddRectFilled(p0, ImVec2(p0.x + 3.5f, p1.y),
                                  Col::Alpha(accent, a), M::CardRadius);

                // A hairline progress bar along the bottom. Knowing
                // it is about to leave is the difference between a
                // notification and a thing stuck on your screen.
                float remain = Clamp01(n.life / n.total);
                if (remain > 0.0f) {
                    dl->AddRectFilled(
                        ImVec2(p0.x, p1.y - 2.0f),
                        ImVec2(p0.x + (width * remain), p1.y),
                        Col::Alpha(accent, 0.45f * a), 1.0f);
                }

                Fonts::Push(Fonts::BodyBold);
                dl->AddText(ImVec2(p0.x + padX, p0.y + padY),
                            Col::Alpha(Col::Label, a), n.title.c_str());
                Fonts::Pop(Fonts::BodyBold);

                if (bs.y > 0.0f) {
                    Fonts::Push(Fonts::Caption);
                    dl->AddText(Fonts::Caption, 0.0f,
                                ImVec2(p0.x + padX, p0.y + padY + ts.y + 4.0f),
                                Col::Alpha(Col::Label2, a),
                                n.body.c_str(), nullptr, wrap);
                    Fonts::Pop(Fonts::Caption);
                }
            }
        }

        // Drop anything that has finished leaving
        while (!s_items.empty() &&
               s_items.front().life <= 0.0f && s_items.front().anim < 0.01f) {
            s_items.pop_front();
        }
    }
};

} // namespace iOS
