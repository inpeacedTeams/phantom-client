#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>
#include <cmath>

class Speed : public Module {
private:
    float m_multiplier = 1.6f;
    int m_mode = 0; // 0 = Strafe, 1 = BHop, 2 = Vanilla
    bool m_groundOnly = false;

    static constexpr float PI = 3.14159265f;

public:
    Speed() : Module("Speed", "Move faster with bypass modes", ModuleCategory::MOVEMENT, 'F') {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        bool onGround = Minecraft::IsOnGround(env, player);
        if (m_groundOnly && !onGround) return;

        float yaw = Minecraft::GetYaw(env, player);

        // Check if player is actually moving (WASD)
        bool moving = (GetAsyncKeyState('W') & 0x8000) ||
                      (GetAsyncKeyState('A') & 0x8000) ||
                      (GetAsyncKeyState('S') & 0x8000) ||
                      (GetAsyncKeyState('D') & 0x8000);
        if (!moving) return;

        switch (m_mode) {
            case 0: { // Strafe
                // Calculate movement direction from keys
                float forward = 0, strafe = 0;
                if (GetAsyncKeyState('W') & 0x8000) forward += 1;
                if (GetAsyncKeyState('S') & 0x8000) forward -= 1;
                if (GetAsyncKeyState('A') & 0x8000) strafe += 1;
                if (GetAsyncKeyState('D') & 0x8000) strafe -= 1;

                float angle = yaw;
                if (forward > 0) {
                    if (strafe > 0) angle -= 45;
                    else if (strafe < 0) angle += 45;
                } else if (forward < 0) {
                    if (strafe > 0) angle -= 135;
                    else if (strafe < 0) angle += 135;
                    else angle += 180;
                } else {
                    if (strafe > 0) angle -= 90;
                    else if (strafe < 0) angle += 90;
                }

                float rad = angle * PI / 180.0f;
                double speed = 0.2873 * m_multiplier;

                Minecraft::SetMotionX(env, player, -sin(rad) * speed);
                Minecraft::SetMotionZ(env, player, cos(rad) * speed);
                break;
            }

            case 1: { // BHop
                if (onGround) {
                    Minecraft::SetMotionY(env, player, 0.4);
                }
                double motX = Minecraft::GetMotionX(env, player);
                double motZ = Minecraft::GetMotionZ(env, player);
                Minecraft::SetMotionX(env, player, motX * m_multiplier);
                Minecraft::SetMotionZ(env, player, motZ * m_multiplier);
                break;
            }

            case 2: { // Vanilla (timer-based, more detectable)
                // Would need timer speed manipulation
                break;
            }
        }
    }

    void RenderSettings() override {
        const char* modes[] = { "Strafe", "BHop", "Vanilla" };
        ImGui::Combo("Mode", &m_mode, modes, 3);
        ImGui::SliderFloat("Multiplier", &m_multiplier, 1.0f, 3.0f, "%.1fx");
        ImGui::Checkbox("Ground Only", &m_groundOnly);
    }
};
