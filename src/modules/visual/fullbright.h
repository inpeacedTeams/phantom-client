#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>

class Fullbright : public Module {
private:
    float m_gamma = 100.0f; // Gamma value when enabled
    float m_savedGamma = 0.0f; // Original gamma to restore

public:
    Fullbright() : Module("Fullbright", "See in complete darkness", ModuleCategory::VISUAL, 'H') {}

    void OnEnable(JNIEnv* env) override {
        // Save current gamma
        // TODO: read current gamma from GameSettings
        m_savedGamma = 1.0f; // default
    }

    void OnTick(JNIEnv* env) override {
        // Set gamma to max every tick (in case game resets it)
        Minecraft::SetGamma(env, m_gamma);
    }

    void OnDisable(JNIEnv* env) override {
        // Restore original gamma
        Minecraft::SetGamma(env, m_savedGamma);
    }

    void RenderSettings() override {
        ImGui::SliderFloat("Gamma", &m_gamma, 1.0f, 1000.0f, "%.0f");
    }
};
