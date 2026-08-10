#pragma once
#include <imgui.h>
#include <cmath>
#include <cstdio>

#include "ios_theme.h"

// =================================================================
// Splash
// =================================================================
// The first thing you see after an inject. Two lines: who made it,
// top left, and how to open the menu, bottom centre. Then it gets
// out of the way.
//
// WHY IT IS NOT JUST A FADE
// A plain alpha ramp reads as a texture being switched on. What
// makes an intro feel built rather than bolted on is that the two
// halves do not move together:
//
//   the title slides in from the left and settles
//   the hint lifts up from below, a beat later
//   both leave the way they came, in the same order
//
// The stagger is the whole trick. Everything arriving at once is a
// popup; things arriving in sequence is an entrance.
//
// TIMING
// Driven by accumulated DeltaTime, not by a frame counter, so it
// runs at the same speed at 60 FPS and at 400. The curve is a cubic
// ease-out for entry, because real motion decelerates into place,
// and a quadratic ease-in on exit so it accelerates away.
//
// THREADING
// Render thread only. Reads nothing but the clock.
// =================================================================

namespace iOS {

class Splash {
private:
    inline static float s_time    = 0.0f;
    inline static bool  s_running = false;
    inline static bool  s_armed   = false;

    // Seconds
    static constexpr float kTitleIn   = 0.55f;
    static constexpr float kHintDelay = 0.28f;   // the stagger
    static constexpr float kHintIn    = 0.55f;
    static constexpr float kHold      = 3.20f;
    static constexpr float kOut       = 0.70f;

    // Thin names over the shared curves in iOS::Ease, so there is
    // one definition of each curve in the client rather than a copy
    // living in every file that animates.
    static float Clamp01(float v) { return iOS::Ease::Clamp01(v); }
    static float EaseOut(float t) { return iOS::Ease::Out(t); }
    static float EaseIn(float t)  { return iOS::Ease::In(t); }

    static float TotalLength() {
        return kHintDelay + kHintIn + kHold + kOut;
    }

    // Text with a soft dark backing, so it stays readable over a
    // bright sky as well as over stone.
    static void Shadowed(ImDrawList* dl, ImVec2 pos, ImU32 col,
                         const char* text, float alpha)
    {
        ImU32 shadow = IM_COL32(0, 0, 0, (int)(150 * alpha));
        dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), shadow, text);
        dl->AddText(pos, col, text);
    }

public:
    // Called once the client is fully up. Safe to call from any
    // thread: it only sets flags, and the render thread starts the
    // clock on its next frame.
    static void Trigger() {
        s_armed = true;
    }

    static void Skip() {
        s_running = false;
        s_armed = false;
    }

    static bool IsPlaying() { return s_running; }

    // -------------------------------------------------------------
    // Render thread, every frame.
    // -------------------------------------------------------------
    static void Render() {
        if (s_armed) {
            s_armed   = false;
            s_running = true;
            s_time    = 0.0f;
        }
        if (!s_running) return;

        ImGuiIO& io = ImGui::GetIO();

        float dt = io.DeltaTime;
        if (dt > 0.1f) dt = 0.1f;      // survive a stall without a jump
        s_time += dt;

        const float total = TotalLength();
        if (s_time >= total) { s_running = false; return; }

        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        if (sw < 2.f || sh < 2.f) return;

        // The exit runs for the last kOut seconds, and both lines
        // share it so they leave together after arriving apart.
        float exitT = Clamp01((s_time - (total - kOut)) / kOut);
        float exit  = EaseIn(exitT);

        // Drawn on the foreground list so nothing in the world or
        // the HUD can end up on top of it.
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        // ---------------------------------------------------------
        // Title, top left
        // ---------------------------------------------------------
        {
            float inT = EaseOut(s_time / kTitleIn);
            float a   = inT * (1.0f - exit);
            if (a > 0.003f) {
                // Comes in from the left, leaves back to the left.
                float slide = (1.0f - inT) * -26.0f + exit * -22.0f;

                Fonts::Push(Fonts::Title);

                const char* text = "| by inpeacedTeam |";
                ImVec2 ts = ImGui::CalcTextSize(text);

                float x = 26.0f + slide;
                float y = 24.0f;

                // A thin accent rule that draws itself in under the
                // text, which gives the eye something to follow.
                float ruleW = ts.x * inT * (1.0f - exit);
                dl->AddRectFilled(
                    ImVec2(x, y + ts.y + 6.0f),
                    ImVec2(x + ruleW, y + ts.y + 8.0f),
                    Col::Alpha(Col::Blue, a * 0.9f), 1.0f);

                Shadowed(dl, ImVec2(x, y),
                         IM_COL32(255, 255, 255, (int)(255 * a)), text, a);

                Fonts::Pop(Fonts::Title);
            }
        }

        // ---------------------------------------------------------
        // Hint, bottom centre
        // ---------------------------------------------------------
        {
            float inT = EaseOut((s_time - kHintDelay) / kHintIn);
            float a   = inT * (1.0f - exit);
            if (a > 0.003f) {
                // Lifts up into place, sinks back down on the way out.
                float rise = (1.0f - inT) * 18.0f + exit * 14.0f;

                Fonts::Push(Fonts::Body);

                const char* text = "| Press Insert to enter the GUI. |";
                ImVec2 ts = ImGui::CalcTextSize(text);

                float x = (sw - ts.x) * 0.5f;
                float y = sh - 78.0f + rise;

                // A soft plate behind it, scaled with the fade so it
                // does not pop in as a hard rectangle.
                float padX = 14.0f, padY = 7.0f;
                dl->AddRectFilled(
                    ImVec2(x - padX, y - padY),
                    ImVec2(x + ts.x + padX, y + ts.y + padY),
                    IM_COL32(0, 0, 0, (int)(96 * a)), 10.0f);

                Shadowed(dl, ImVec2(x, y),
                         IM_COL32(235, 238, 245, (int)(255 * a)), text, a);

                Fonts::Pop(Fonts::Body);
            }
        }
    }
};

} // namespace iOS
