#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <random>

// =================================================================
// Sprint Reset (More KB)
// =================================================================
// Every hit while sprinting deals extra knockback, but the server
// cancels your sprint after the first one (MC-69459). The client
// still thinks it is sprinting, so every hit after that lands with
// reduced knockback without you noticing.
//
// Sprint resetting means dropping and re-applying sprint so each
// hit counts as a fresh sprint hit.
//
// IMPLEMENTATION
// We toggle real KeyBindings rather than writing moveForward or
// calling setSprinting directly. Our thread is not synchronised
// with the game tick, so writing movement fields is a race we lose
// half the time. Releasing keyBindForward is exactly what a human
// w-tap does, and Minecraft picks it up during its own input pass.
//
// METHODS
//   0 W-Tap     release forward for a tick
//   1 S-Tap     tap back while forward stays held
//   2 Blockhit  hold use-item, which also resets sprint
//   3 Sneak Tap tap sneak, no speed loss
//   4 Ctrl Spam re-press the sprint key
//   5 Packet    setSprinting(false) then (true) in one tick
// =================================================================

class SprintReset : public Module {
private:
    int   m_method          = 0;
    float m_chance          = 100.0f;
    int   m_resetTicksMin   = 1;
    int   m_resetTicksMax   = 1;
    bool  m_onlyWhileMoving = true;
    int   m_hitDelay        = 0;

    // State
    bool m_isResetting     = false;
    int  m_resetCountdown  = 0;
    int  m_delayCountdown  = 0;
    bool m_waitingForDelay = false;
    bool m_lastLMB         = false;
    bool m_keyWasSet       = false;

    jmethodID m_setSprinting = nullptr;
    bool m_resolved = false;

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

    int RandTicks() {
        if (m_resetTicksMin >= m_resetTicksMax) return m_resetTicksMin;
        std::uniform_int_distribution<int> d(m_resetTicksMin, m_resetTicksMax);
        return d(m_rng);
    }

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::entity) {
            m_setSprinting = JvmtiUtil::FindMethod(env, ClassResolver::entity,
                { "func_70031_b", "setSprinting" }, 1);
        }
        m_resolved = true;
    }

    void EndReset(JNIEnv* env, jobject player) {
        switch (m_method) {
            case 0: Minecraft::SetKeyPressed(env, GameKey::Forward, true);  break;
            case 1: Minecraft::SetKeyPressed(env, GameKey::Back,    false); break;
            case 2: Minecraft::SetKeyPressed(env, GameKey::UseItem, false); break;
            case 3: Minecraft::SetKeyPressed(env, GameKey::Sneak,   false); break;
            case 4: Minecraft::SetKeyPressed(env, GameKey::Sprint,  true);  break;
            default: break;
        }

        // Re-apply sprint for the methods that dropped it outright
        if (m_setSprinting && player &&
            (m_method == 0 || m_method == 1 || m_method == 4)) {
            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        m_isResetting = false;
        m_keyWasSet   = false;
    }

    void BeginReset(JNIEnv* env, jobject player) {
        switch (m_method) {
            case 0: Minecraft::SetKeyPressed(env, GameKey::Forward, false); break;
            case 1: Minecraft::SetKeyPressed(env, GameKey::Back,    true);  break;
            case 2: Minecraft::SetKeyPressed(env, GameKey::UseItem, true);  break;
            case 3: Minecraft::SetKeyPressed(env, GameKey::Sneak,   true);  break;
            case 4: Minecraft::SetKeyPressed(env, GameKey::Sprint,  false); break;
            case 5:
                if (m_setSprinting) {
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                return;   // instant, nothing to unwind
        }

        if (m_setSprinting && (m_method == 0 || m_method == 1 || m_method == 4)) {
            env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        m_isResetting    = true;
        m_keyWasSet      = true;
        m_resetCountdown = RandTicks();
    }

public:
    SprintReset()
        : Module("Sprint Reset", "Reset sprint on every hit for full knockback",
                 ModuleCategory::COMBAT, 0) {}

    void OnTick(JNIEnv* env) override {
        Resolve(env);

        if (!Minecraft::HasKeyBinds() && m_method != 5) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (Minecraft::IsInGui(env)) {
            if (m_isResetting) EndReset(env, player);
            return;
        }

        // Finish an in-flight reset first
        if (m_isResetting) {
            if (m_resetCountdown <= 0) EndReset(env, player);
            else m_resetCountdown--;
            return;
        }

        if (m_onlyWhileMoving && !(GetAsyncKeyState('W') & 0x8000)) return;

        if (m_waitingForDelay) {
            if (m_delayCountdown > 0) { m_delayCountdown--; return; }
            m_waitingForDelay = false;
            if (Roll(m_chance)) BeginReset(env, player);
            return;
        }

        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool justClicked = lmb && !m_lastLMB;
        m_lastLMB = lmb;
        if (!justClicked) return;

        if (m_hitDelay > 0) {
            m_delayCountdown  = m_hitDelay;
            m_waitingForDelay = true;
            return;
        }

        if (Roll(m_chance)) BeginReset(env, player);
    }

    void OnDisable(JNIEnv* env) override {
        if (!m_keyWasSet && !m_isResetting) return;
        jobject player = Minecraft::GetPlayer(env);
        EndReset(env, player);
        m_waitingForDelay = false;
        m_resetCountdown  = 0;
    }

    void RenderSettings() override {
        const char* methods[] = {
            "W-Tap", "S-Tap", "Blockhit", "Sneak Tap", "Ctrl Spam", "Packet"
        };
        ImGui::Combo("Method", &m_method, methods, 6);

        if (m_method == 5) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "! Packet mode is detected by Polar and AGC");
        }

        ImGui::Separator();
        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::SliderInt("Reset Ticks Min", &m_resetTicksMin, 1, 5);
        ImGui::SliderInt("Reset Ticks Max", &m_resetTicksMax, 1, 5);
        if (m_resetTicksMin > m_resetTicksMax) m_resetTicksMin = m_resetTicksMax;
        ImGui::SliderInt("Hit Delay (ticks)", &m_hitDelay, 0, 5);
        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);

        ImGui::Separator();
        switch (m_method) {
            case 0: ImGui::TextWrapped("Releases forward for a tick. The standard reset, works everywhere."); break;
            case 1: ImGui::TextWrapped("Taps back for a tick. Stops you faster and opens more distance."); break;
            case 2: ImGui::TextWrapped("Holds block. Resets sprint and halves incoming damage. Sword only."); break;
            case 3: ImGui::TextWrapped("Taps sneak. No speed loss, common in sumo."); break;
            case 4: ImGui::TextWrapped("Re-presses the sprint key. Loses the least momentum."); break;
            case 5: ImGui::TextWrapped("Toggles sprint within a single tick. Fastest, but detectable."); break;
        }

        if (!Minecraft::HasKeyBinds()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "KeyBindings unresolved: only Packet mode works");
        }
    }
};
