#pragma once
#include <imgui.h>
#include <chrono>
#include <cmath>
#include <atomic>

#include "ios_theme.h"

// =================================================================
// Welcome
// =================================================================
// The first second after injecting is the only confirmation the
// client ever gives that it loaded. A console window behind the
// game is not confirmation; you cannot see it.
//
// Two pieces of text, both driven by one clock:
//
//   top left     | by inpeacedTeam |
//   bottom       | Press Insert to enter the GUI. |
//
// TIMING IS THE WHOLE THING
// A label that snaps on and snaps off looks like a bug. The shape
// used here is the one every good client uses: come in fast, sit
// still long enough to be read, leave slowly.
//
//   0.00-0.45  slide and fade in, decelerating
//   0.45-3.60  hold
//   3.60-4.40  fade out, drifting slightly further
//
// The two lines are offset by 120ms so they arrive as a sequence
// rather than a single pop. That stagger is most of why it reads as
// deliberate.
//
// FRAME RATE
// Everything is a function of elapsed time, never of frame count,
// so it looks identical at 60 and 400 FPS.
//
// THREADING
// Trigger() is called from the client thread once the injection
// finished. Render() runs on the render thread. One atomic between
// them, and the clock is only ever read on the render side.
// =================================================================

class Welcome {
private:
    using Clock = std::chrono::steady_clock;

    inline static std::atomic<bool> s_requested{ false };
    inline static bool  s_running = false;
    inline static Clock::time_point s_start;

    // Seconds
    static constexpr float kIn    = 0.45f;
    static constexpr float kHold  = 3.15f;
    static constexpr float kOut   = 0.80f;
    static constexpr float kStagger = 0.12f;
    static constexpr float kTotal = kIn + kHold + kOut + kStagger + 0.1f;

    static float Clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // Decelerating. Fast off the mark, settles softly.
    static float EaseOut(float t) {
        float u = 1.0f - Clamp01(t);
        return 1.0f - u * u * u;
    }

    // Accelerating, for the exit. Leaving should start gently and
    // then get out of the way.
    static float EaseIn(float t) {
        t = Clamp01(t);
        return t * t;
    }

    struct Phase {
        float alpha  = 0.0f;
        float offset = 0.0f;   // pixels of slide remaining
        bool  alive  = false;
    };

    static Phase Evaluate(float age, float delay, float slide) {
        Phase p;
        float t = age - delay;
        if (t < 0.0f) { p.alive = true; return p; }   // not started yet

        if (t < kIn) {
            float e = EaseOut(t / kIn);
            p.alpha  = e;
            p.offset = slide * (1.0f - e);
            p.alive  = true;
        } else if (t < kIn + kHold) {
            p.alpha  = 1.0f;
            p.offset = 0.0f;
            p.alive  = true;
        } else if (t < kIn + kHold + kOut) {
            float e = EaseIn((t - kIn - kHold) / kOut);
            p.alpha  = 1.0f - e;
            // Drifts a little further in the same direction on the
            // way out, which reads as leaving rather than blinking.
            p.offset = -slide * 0.45f * e;
            p.alive  = true;
        }
        return p;
    }

    // Pill with a hairline and a soft shadow. Same language as the
    // menu, so the first thing you see belongs to the same client.
    static void DrawPill(ImDrawList* dl, ImVec2 centre, const char* text,
                         float alpha, bool accent)
    {
        if (alpha <= 0.004f) return;

        ImVec2 ts = ImGui::CalcTextSize(text);
        float padX = 16.0f, padY = 9.0f;
        float w = ts.x + padX * 2.0f;
        float h = ts.y + padY * 2.0f;
        float r = h * 0.5f;

        ImVec2 a(centre.x - w * 0.5f, centre.y - h * 0.5f);
        ImVec2 b(a.x + w, a.y + h);

        int sa = (int)(46 * alpha);
        dl->AddRectFilled(ImVec2(a.x, a.y + 3.0f), ImVec2(b.x, b.y + 5.0f),
                          IM_COL32(0, 0, 0, sa), r);

        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, (int)(242 * alpha)), r);
        dl->AddRect(a, b, IM_COL32(0, 0, 0, (int)(18 * alpha)), r, 0, 1.0f);

        ImU32 col = accent
            ? IM_COL32(0, 122, 255, (int)(255 * alpha))
            : IM_COL32(30, 30, 34, (int)(230 * alpha));

        dl->AddText(ImVec2(a.x + padX, a.y + padY), col, text);
    }

public:
    // Client thread. Safe to call more than once; only the first
    // call after a reset does anything.
    static void Trigger() { s_requested.store(true, std::memory_order_release); }

    static void Reset() {
        s_requested.store(false);
        s_running = false;
    }

    static bool IsPlaying() { return s_running; }

    // Render thread, every frame.
    static void Render() {
        if (s_requested.exchange(false, std::memory_order_acquire)) {
            s_running = true;
            s_start = Clock::now();
        }
        if (!s_running) return;

        float age = std::chrono::duration<float>(Clock::now() - s_start).count();
        if (age > kTotal) { s_running = false; return; }

        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.f || sh < 2.f) return;

        auto* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        iOS::Fonts::Push(iOS::Fonts::BodyBold);

        // ---- Top left: who made it ----
        {
            Phase p = Evaluate(age, 0.0f, 26.0f);
            if (p.alpha > 0.004f) {
                const char* text = "| by inpeacedTeam |";
                ImVec2 ts = ImGui::CalcTextSize(text);
                float w = ts.x + 32.0f;
                // Slides in from the left, so the offset is negative
                DrawPill(dl,
                    ImVec2(26.0f + w * 0.5f - p.offset, 34.0f),
                    text, p.alpha, true);
            }
        }

        iOS::Fonts::Pop(iOS::Fonts::BodyBold);
        iOS::Fonts::Push(iOS::Fonts::Body);

        // ---- Bottom centre: how to open it ----
        {
            Phase p = Evaluate(age, kStagger, 18.0f);
            if (p.alpha > 0.004f) {
                DrawPill(dl,
                    ImVec2(sw * 0.5f, sh - 78.0f + p.offset),
                    "| Press Insert to enter the GUI. |", p.alpha, false);
            }
        }

        iOS::Fonts::Pop(iOS::Fonts::Body);
    }
};
