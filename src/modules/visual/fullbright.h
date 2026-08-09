#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include <imgui.h>

// =================================================================
// Fullbright
// =================================================================
// Raises GameSettings.gammaSetting far past the slider maximum so
// dark areas render fully lit. Purely client-side rendering state,
// so no server ever sees it.
// =================================================================

class Fullbright : public Module {
private:
    float m_gamma      = 100.0f;
    float m_savedGamma = 1.0f;
    bool  m_saved      = false;

public:
    Fullbright() : Module("Fullbright", "See in complete darkness",
                          ModuleCategory::VISUAL, 'H') {}

    void OnEnable(JNIEnv* env) override {
        // Capture whatever the user actually had, so disabling puts
        // their brightness back instead of guessing 1.0.
        m_savedGamma = Minecraft::GetGamma(env);
        m_saved = true;
    }

    void OnTick(JNIEnv* env) override {
        // Reapplied every tick because the options screen writes the
        // slider value back over ours whenever it is open.
        Minecraft::SetGamma(env, m_gamma);
    }

    void OnDisable(JNIEnv* env) override {
        if (m_saved) Minecraft::SetGamma(env, m_savedGamma);
    }

    void RenderSettings() override {
        ImGui::SliderFloat("Gamma", &m_gamma, 1.0f, 1000.0f, "%.0f");
        if (m_saved) ImGui::TextDisabled("Original gamma: %.2f", m_savedGamma);
    }
};
