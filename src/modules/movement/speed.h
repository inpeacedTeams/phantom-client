#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include <imgui.h>
#include <cmath>

// =================================================================
// Speed
// =================================================================
// Overwrites the horizontal motion vector directly. There is no
// subtle version of this: a prediction anticheat simulates your
// movement and compares, so any multiplier shows up immediately.
// Unprotected servers only.
//
// Input is read through KeyBinds rather than raw scancodes, so a
// player who rebound WASD still gets the right direction.
//
// MODES
//   0 Strafe  recompute the motion vector from the held direction
//   1 BHop    hop on every landing and scale the existing motion
// =================================================================

class Speed : public Module {
private:
    float m_multiplier = 1.6f;
    int   m_mode       = 0;
    bool  m_groundOnly = false;

    static constexpr double kBaseSpeed = 0.2873;   // vanilla sprint

public:
    Speed() : Module("Speed", "Move faster. Detected by prediction anticheats",
                     ModuleCategory::MOVEMENT, 'F')
    {
        Bind("Mode", &m_mode);
        Bind("Multiplier", &m_multiplier);
        Bind("Ground Only", &m_groundOnly);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        bool onGround = Minecraft::IsOnGround(env, player);
        if (m_groundOnly && !onGround) return;

        bool fwd   = KeyBinds::GetForward(env);
        bool back  = KeyBinds::GetBack(env);
        bool left  = KeyBinds::GetLeft(env);
        bool right = KeyBinds::GetRight(env);
        if (!fwd && !back && !left && !right) return;

        if (m_mode == 1) {   // BHop
            if (onGround) Minecraft::SetMotionY(env, player, 0.4);
            double mx = Minecraft::GetMotionX(env, player);
            double mz = Minecraft::GetMotionZ(env, player);
            Minecraft::SetMotionX(env, player, mx * m_multiplier);
            Minecraft::SetMotionZ(env, player, mz * m_multiplier);
            return;
        }

        // Strafe: rebuild the vector from the direction being held.
        // Diagonals are 45 degrees off the facing angle, exactly as
        // vanilla movement resolves them.
        float forward = (fwd ? 1.f : 0.f) - (back ? 1.f : 0.f);
        float strafe  = (left ? 1.f : 0.f) - (right ? 1.f : 0.f);

        float angle = Minecraft::GetYaw(env, player);
        if (forward > 0.f) {
            if (strafe > 0.f)      angle -= 45.f;
            else if (strafe < 0.f) angle += 45.f;
        } else if (forward < 0.f) {
            if (strafe > 0.f)      angle -= 135.f;
            else if (strafe < 0.f) angle += 135.f;
            else                   angle += 180.f;
        } else {
            if (strafe > 0.f)      angle -= 90.f;
            else if (strafe < 0.f) angle += 90.f;
        }

        double rad   = angle * 3.14159265358979 / 180.0;
        double speed = kBaseSpeed * m_multiplier;

        Minecraft::SetMotionX(env, player, -std::sin(rad) * speed);
        Minecraft::SetMotionZ(env, player,  std::cos(rad) * speed);
    }

    void RenderSettings() override {
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f),
            "! Caught instantly by AGC, Grim and Polar");
        ImGui::Separator();

        const char* modes[] = { "Strafe", "BHop" };
        ImGui::Combo("Mode", &m_mode, modes, 2);
        ImGui::SliderFloat("Multiplier", &m_multiplier, 1.0f, 3.0f, "%.2fx");
        ImGui::Checkbox("Ground Only", &m_groundOnly);
    }
};
