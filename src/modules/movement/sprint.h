#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mappings/mcp189.h"
#include <imgui.h>

class Sprint : public Module {
private:
    bool m_omniSprint = false; // Sprint in all directions

    jmethodID m_setSprinting = nullptr;
    bool m_resolved = false;

    void ResolveMethod(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::entity) {
            // Entity.setSprinting(boolean)
            m_setSprinting = env->GetMethodID(ClassResolver::entity, "func_70031_b", "(Z)V");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                m_setSprinting = env->GetMethodID(ClassResolver::entity, "setSprinting", "(Z)V");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        m_resolved = true;
    }

public:
    Sprint() : Module("Sprint", "Always sprint without holding key", ModuleCategory::MOVEMENT, VK_CONTROL) {}

    void OnTick(JNIEnv* env) override {
        ResolveMethod(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        // Check if moving forward (or any direction if omniSprint)
        bool moving;
        if (m_omniSprint) {
            moving = (GetAsyncKeyState('W') & 0x8000) ||
                     (GetAsyncKeyState('A') & 0x8000) ||
                     (GetAsyncKeyState('S') & 0x8000) ||
                     (GetAsyncKeyState('D') & 0x8000);
        } else {
            moving = (GetAsyncKeyState('W') & 0x8000);
        }

        if (moving && m_setSprinting) {
            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    void RenderSettings() override {
        ImGui::Checkbox("Omni Sprint", &m_omniSprint);
    }
};
