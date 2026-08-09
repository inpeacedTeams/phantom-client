#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include <imgui.h>
#include <Windows.h>
#include <random>

// =================================================================
// Sprint Reset (More KB)
// =================================================================
// The server cancels your sprint after the first hit you land
// (MC-69459). Your client still thinks it is sprinting, so every
// following hit deals reduced knockback without you noticing.
// Manually un-sprinting and re-sprinting makes every hit a full
// sprint hit again.
//
// IMPLEMENTATION NOTE
// An earlier version called entity.setSprinting(false). That does
// nothing: EntityPlayerSP.onLivingUpdate() recomputes the sprint
// state from the movement keys every tick and overwrites it before
// the packet is sent. We drive the KeyBindings instead, which sits
// upstream of that logic and produces the exact packet stream a
// real key release produces.
//
// METHODS
//   0 W-Tap     release forward for a tick. Most common
//   1 S-Tap     hold back for a tick. Stops you faster
//   2 Blockhit  right-click block. Resets sprint and halves damage
//   3 Sneak Tap crouch for a tick. No speed loss
//   4 Ctrl Spam release the sprint key itself. Least momentum lost
// =================================================================

class SprintReset : public Module {
private:
    int   m_method          = 0;
    float m_chance          = 100.0f;
    int   m_resetTicksMin   = 1;
    int   m_resetTicksMax   = 1;
    int   m_hitDelay        = 0;
    bool  m_onlyWhileMoving = true;
    bool  m_fullSTap        = false;  // S-Tap: also release forward

    // State
    bool m_resetting        = false;
    int  m_resetCountdown   = 0;
    int  m_delayCountdown   = 0;
    bool m_waitingDelay     = false;
    bool m_lastLMB          = false;
    bool m_savedForward     = false;  // key state before we took over

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

    void BeginReset(JNIEnv* env) {
        m_savedForward = KeyBinds::GetForward(env);

        switch (m_method) {
            case 0:  // W-Tap
                KeyBinds::SetForward(env, false);
                break;
            case 1:  // S-Tap
                KeyBinds::SetBack(env, true);
                if (m_fullSTap) KeyBinds::SetForward(env, false);
                break;
            case 2:  // Blockhit
                KeyBinds::SetUseItem(env, true);
                break;
            case 3:  // Sneak Tap
                KeyBinds::SetSneak(env, true);
                break;
            case 4:  // Ctrl Spam
                KeyBinds::SetSprint(env, false);
                break;
        }

        m_resetting = true;
        m_resetCountdown = RandTicks();
    }

    void EndReset(JNIEnv* env) {
        switch (m_method) {
            case 0:
                // Hand the key back in the state the player left it
                KeyBinds::SetForward(env, m_savedForward);
                break;
            case 1:
                KeyBinds::SetBack(env, false);
                if (m_fullSTap) KeyBinds::SetForward(env, m_savedForward);
                break;
            case 2:
                KeyBinds::SetUseItem(env, false);
                break;
            case 3:
                KeyBinds::SetSneak(env, false);
                break;
            case 4:
                KeyBinds::SetSprint(env, true);
                break;
        }
        m_resetting = false;
    }

public:
    SprintReset()
        : Module("Sprint Reset", "Full sprint knockback on every hit",
                 ModuleCategory::COMBAT, 0)
    {
        Bind("Method", &m_method);
        Bind("Chance", &m_chance);
        Bind("Reset Ticks Min", &m_resetTicksMin);
        Bind("Reset Ticks Max", &m_resetTicksMax);
        Bind("Hit Delay", &m_hitDelay);
        Bind("Only While Moving", &m_onlyWhileMoving);
        Bind("Full S-Tap", &m_fullSTap);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) {
            if (m_resetting) EndReset(env);
            return;
        }

        // ---- Finish an in-flight reset ----
        if (m_resetting) {
            if (m_resetCountdown <= 0) EndReset(env);
            else m_resetCountdown--;
            return;
        }

        // ---- Optional delay between the hit and the reset ----
        if (m_waitingDelay) {
            if (m_delayCountdown > 0) { m_delayCountdown--; return; }
            m_waitingDelay = false;
            if (Roll(m_chance)) BeginReset(env);
            return;
        }

        if (m_onlyWhileMoving && !KeyBinds::GetForward(env)) return;

        // ---- Detect our own swing ----
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool justClicked = lmb && !m_lastLMB;
        m_lastLMB = lmb;
        if (!justClicked) return;

        if (m_hitDelay > 0) {
            m_delayCountdown = m_hitDelay;
            m_waitingDelay = true;
            return;
        }

        if (Roll(m_chance)) BeginReset(env);
    }

    void OnDisable(JNIEnv* env) override {
        if (m_resetting) EndReset(env);
        m_waitingDelay = false;
    }

    void RenderSettings() override {
        const char* methods[] = {
            "W-Tap", "S-Tap", "Blockhit", "Sneak Tap", "Ctrl Spam"
        };
        ImGui::Combo("Method", &m_method, methods, 5);

        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::SliderInt("Reset Ticks Min", &m_resetTicksMin, 1, 5);
        ImGui::SliderInt("Reset Ticks Max", &m_resetTicksMax, 1, 5);
        if (m_resetTicksMin > m_resetTicksMax) m_resetTicksMin = m_resetTicksMax;
        ImGui::SliderInt("Hit Delay (ticks)", &m_hitDelay, 0, 5);
        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);
        if (m_method == 1) ImGui::Checkbox("Full S-Tap", &m_fullSTap);

        ImGui::Separator();
        switch (m_method) {
            case 0: ImGui::TextWrapped("Releases forward for a tick. Clean and universal."); break;
            case 1: ImGui::TextWrapped("Taps back. Stops you faster, best on high ping."); break;
            case 2: ImGui::TextWrapped("Blocks with the sword. Resets sprint and halves damage."); break;
            case 3: ImGui::TextWrapped("Crouches for a tick. Keeps your speed."); break;
            case 4: ImGui::TextWrapped("Releases the sprint key. Loses the least momentum."); break;
        }

        if (!KeyBinds::HasMovement()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Keybinds unresolved: module inactive");
        }
    }
};
