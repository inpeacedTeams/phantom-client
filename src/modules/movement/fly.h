#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>
#include <cmath>

class Fly : public Module {
private:
    float m_speed = 2.0f;
    int m_mode = 0; // 0 = Vanilla, 1 = Glide
    bool m_antiKick = true; // Descend slightly to avoid fly kick

    int m_tickCounter = 0;

public:
    Fly() : Module("Fly", "Fly in survival mode", ModuleCategory::MOVEMENT, 'G') {}

    void OnTick(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        m_tickCounter++;

        float yaw = Minecraft::GetYaw(env, player);
        float pitch = Minecraft::GetPitch(env, player);
        float rad = yaw * 3.14159265f / 180.0f;

        switch (m_mode) {
            case 0: { // Vanilla fly
                double motionY = 0.0;

                if (GetAsyncKeyState(VK_SPACE) & 0x8000) motionY = m_speed * 0.15;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) motionY = -m_speed * 0.15;

                // Anti-kick: slightly descend every 40 ticks
                if (m_antiKick && m_tickCounter % 40 == 0) {
                    motionY = -0.04;
                }

                Minecraft::SetMotionY(env, player, motionY);

                // Forward/backward
                bool moving = (GetAsyncKeyState('W') & 0x8000) ||
                              (GetAsyncKeyState('S') & 0x8000) ||
                              (GetAsyncKeyState('A') & 0x8000) ||
                              (GetAsyncKeyState('D') & 0x8000);

                if (moving) {
                    double speed = m_speed * 0.15;
                    if (GetAsyncKeyState('W') & 0x8000) {
                        Minecraft::SetMotionX(env, player, -sin(rad) * speed);
                        Minecraft::SetMotionZ(env, player, cos(rad) * speed);
                    }
                    if (GetAsyncKeyState('S') & 0x8000) {
                        Minecraft::SetMotionX(env, player, sin(rad) * speed);
                        Minecraft::SetMotionZ(env, player, -cos(rad) * speed);
                    }
                } else {
                    Minecraft::SetMotionX(env, player, 0);
                    Minecraft::SetMotionZ(env, player, 0);
                }
                break;
            }

            case 1: { // Glide
                double motionY = Minecraft::GetMotionY(env, player);
                if (motionY < 0) {
                    Minecraft::SetMotionY(env, player, -0.01); // Slow fall
                }
                break;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        // Reset motion on disable
        jobject player = Minecraft::GetPlayer(env);
        if (player) {
            Minecraft::SetMotionY(env, player, 0);
        }
    }

    void RenderSettings() override {
        const char* modes[] = { "Vanilla", "Glide" };
        ImGui::Combo("Mode", &m_mode, modes, 2);
        ImGui::SliderFloat("Speed", &m_speed, 0.5f, 5.0f, "%.1f");
        ImGui::Checkbox("Anti Kick", &m_antiKick);
    }
};
