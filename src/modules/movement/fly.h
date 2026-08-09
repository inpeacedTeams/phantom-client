#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include <imgui.h>
#include <cmath>

// =================================================================
// Fly
// =================================================================
// Writes the motion vector every tick. Every prediction anticheat
// catches this on the first airborne tick. Unprotected servers only.
//
// The old version only handled forward and back, so strafing while
// flying dropped you out of the air. It also read raw scancodes,
// which broke for anyone with rebound movement.
//
// MODES
//   0 Vanilla  full directional flight
//   1 Glide    cancel most of the fall speed, nothing else
// =================================================================

class Fly : public Module {
private:
    float m_speed     = 2.0f;
    int   m_mode      = 0;
    bool  m_antiKick  = true;
    int   m_kickEvery = 40;    // ticks between the corrective dip

    int m_tick = 0;

public:
    Fly() : Module("Fly", "Fly in survival. Detected by prediction anticheats",
                   ModuleCategory::MOVEMENT, 'G')
    {
        Bind("Mode", &m_mode);
        Bind("Speed", &m_speed);
        Bind("Anti Kick", &m_antiKick);
        Bind("Kick Interval", &m_kickEvery);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        m_tick++;

        if (m_mode == 1) {   // Glide
            if (Minecraft::GetMotionY(env, player) < 0.0)
                Minecraft::SetMotionY(env, player, -0.01);
            return;
        }

        // ---- Vertical ----
        double my = 0.0;
        if (KeyBinds::GetJump(env))  my =  m_speed * 0.15;
        if (KeyBinds::GetSneak(env)) my = -m_speed * 0.15;

        // Servers kick players who hold a constant altitude in air.
        // A brief dip resets that counter.
        if (m_antiKick && m_kickEvery > 0 && (m_tick % m_kickEvery) == 0)
            my = -0.04;

        Minecraft::SetMotionY(env, player, my);

        // ---- Horizontal ----
        bool fwd   = KeyBinds::GetForward(env);
        bool back  = KeyBinds::GetBack(env);
        bool left  = KeyBinds::GetLeft(env);
        bool right = KeyBinds::GetRight(env);

        if (!fwd && !back && !left && !right) {
            Minecraft::SetMotionX(env, player, 0.0);
            Minecraft::SetMotionZ(env, player, 0.0);
            return;
        }

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
        double speed = m_speed * 0.15;

        Minecraft::SetMotionX(env, player, -std::sin(rad) * speed);
        Minecraft::SetMotionZ(env, player,  std::cos(rad) * speed);
    }

    void OnDisable(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (player) Minecraft::SetMotionY(env, player, 0.0);
        m_tick = 0;
    }

    void RenderSettings() override {
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f),
            "! Caught instantly by AGC, Grim and Polar");
        ImGui::Separator();

        const char* modes[] = { "Vanilla", "Glide" };
        ImGui::Combo("Mode", &m_mode, modes, 2);
        ImGui::SliderFloat("Speed", &m_speed, 0.5f, 5.0f, "%.1f");

        if (m_mode == 0) {
            ImGui::Checkbox("Anti Kick", &m_antiKick);
            if (m_antiKick)
                ImGui::SliderInt("Kick Interval", &m_kickEvery, 10, 100);
        }
    }
};
