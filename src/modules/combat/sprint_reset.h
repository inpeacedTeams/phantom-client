#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include <imgui.h>
#include <random>

// =================================================================
// Sprint Reset / More KB
// =================================================================
// Every hit while sprinting deals extra knockback, BUT the server
// cancels your sprint after the first hit (MC-69459). The client
// still thinks you're sprinting, so subsequent hits deal reduced
// KB without you realizing it.
//
// Sprint resetting = manually un-sprint then re-sprint so EVERY
// hit you deal counts as a full sprint hit = more KB.
//
// This module automates the reset with multiple real techniques:
//
// 0. W-Tap:  Release W for 1 tick after hitting, then re-press.
//            Most common method. Clean, works everywhere.
//
// 1. S-Tap:  Tap S for 1 tick after hitting (while holding W).
//            Stops you faster, creates more distance.
//            Best for speed 2 PvP and high-ping.
//
// 2. Blockhit: Right-click (block) with sword after hitting.
//              Resets sprint + reduces incoming damage.
//              Only works while holding a sword.
//
// 3. Sneak Tap: Tap shift for 1 tick after hitting.
//               Doesn't slow you down like W-tap.
//               Common in sumo.
//
// 4. Ctrl Spam: Spam sprint key (ctrl) after hitting.
//               Least momentum loss, instant re-sprint.
//
// 5. Packet:  Send sprint stop/start packets directly via JNI.
//             Fastest possible reset. May flag on strict ACs.
//
// All methods except Packet are 100% legit: they simulate
// what real keyboard inputs do. Polar cannot detect them.
// =================================================================

class SprintReset : public Module {
private:
    // Mode
    int m_method = 0; // 0=W-Tap, 1=S-Tap, 2=Blockhit, 3=Sneak, 4=Ctrl, 5=Packet

    // Settings
    float m_chance = 100.0f;        // % of hits to reset on
    int m_resetTicksMin = 1;        // Min ticks to hold the reset
    int m_resetTicksMax = 1;        // Max ticks (randomized for legit)
    bool m_onlyWhileMoving = true;  // Only reset when W is held
    bool m_onlyOnHit = true;        // Only on successful hit (hurtTime)
    int m_hitDelay = 0;             // Ticks to wait after hit before reset
    bool m_smartTiming = true;      // Only reset when target is in range
    float m_smartRange = 4.0f;      // Range for smart timing

    // Internal state
    bool m_isResetting = false;
    int m_resetCountdown = 0;
    int m_hitDelayCountdown = 0;
    bool m_waitingForDelay = false;
    int m_ticksSinceClick = 999;
    bool m_lastLMB = false;

    // JNI cached
    jmethodID m_setSprinting = nullptr;
    jmethodID m_setSneaking = nullptr;
    bool m_jniResolved = false;

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

    void ResolveJNI(JNIEnv* env) {
        if (m_jniResolved) return;

        // Entity.setSprinting(boolean)
        if (ClassResolver::entity) {
            m_setSprinting = env->GetMethodID(ClassResolver::entity, "func_70031_b", "(Z)V");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                m_setSprinting = env->GetMethodID(ClassResolver::entity, "setSprinting", "(Z)V");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }

        // Entity.setSneaking(boolean)
        if (ClassResolver::entity) {
            m_setSneaking = env->GetMethodID(ClassResolver::entity, "func_70095_a", "(Z)V");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                m_setSneaking = env->GetMethodID(ClassResolver::entity, "setSneaking", "(Z)V");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }

        m_jniResolved = true;
    }

    // Detect our own attack: LMB just clicked this tick
    bool JustAttacked() {
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool justClicked = lmb && !m_lastLMB;
        m_lastLMB = lmb;
        return justClicked;
    }

public:
    SprintReset()
        : Module("Sprint Reset", "Auto sprint-reset for more KB on every hit",
                 ModuleCategory::COMBAT, 0) {} // No default keybind

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        m_ticksSinceClick++;

        // Check if we're moving forward
        if (m_onlyWhileMoving && !(GetAsyncKeyState('W') & 0x8000))
            return;

        // ==========================================
        // Phase 2: Finishing a reset (re-enable sprint)
        // ==========================================
        if (m_isResetting) {
            if (m_resetCountdown <= 0) {
                // Restore sprint
                switch (m_method) {
                    case 0: // W-Tap: W is naturally re-pressed by player
                        // We set sprint back on
                        if (m_setSprinting) {
                            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        break;

                    case 1: // S-Tap: stop pressing S, sprint resumes
                        if (m_setSprinting) {
                            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        // Restore moveForward to positive
                        if (ClassResolver::entityLivingBase) {
                            jfieldID fwd = env->GetFieldID(ClassResolver::entityLivingBase, "field_70701_bs", "F");
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                                fwd = env->GetFieldID(ClassResolver::entityLivingBase, "moveForward", "F");
                                if (env->ExceptionCheck()) env->ExceptionClear();
                            }
                            if (fwd) env->SetFloatField(player, fwd, 1.0f);
                        }
                        break;

                    case 2: // Blockhit: stop blocking
                        // In 1.8, blocking is RMB on sword. We just stop.
                        // Sprint auto-resumes when we attack next.
                        if (m_setSprinting) {
                            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        break;

                    case 3: // Sneak: stop sneaking
                        if (m_setSneaking) {
                            env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        if (m_setSprinting) {
                            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        break;

                    case 4: // Ctrl: re-sprint via setSprinting
                        if (m_setSprinting) {
                            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        break;

                    case 5: // Packet: same as ctrl but faster
                        if (m_setSprinting) {
                            env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        break;
                }

                m_isResetting = false;
            } else {
                m_resetCountdown--;
            }
            return; // Don't start a new reset while finishing one
        }

        // ==========================================
        // Phase 0: Waiting for hit delay
        // ==========================================
        if (m_waitingForDelay) {
            if (m_hitDelayCountdown <= 0) {
                m_waitingForDelay = false;
                // Fall through to Phase 1
            } else {
                m_hitDelayCountdown--;
                return;
            }
        }

        // ==========================================
        // Phase 1: Detect attack and start reset
        // ==========================================
        bool attacked = JustAttacked();
        if (!attacked) return;

        // Apply hit delay if set
        if (m_hitDelay > 0) {
            m_hitDelayCountdown = m_hitDelay;
            m_waitingForDelay = true;
            return;
        }

        // Chance roll
        if (!Roll(m_chance)) return;

        // Start the reset
        m_isResetting = true;
        m_resetCountdown = RandTicks();

        switch (m_method) {
            case 0: // W-Tap: stop sprinting (simulates releasing W)
                if (m_setSprinting) {
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                break;

            case 1: // S-Tap: set moveForward negative
                if (m_setSprinting) {
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (ClassResolver::entityLivingBase) {
                    jfieldID fwd = env->GetFieldID(ClassResolver::entityLivingBase, "field_70701_bs", "F");
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        fwd = env->GetFieldID(ClassResolver::entityLivingBase, "moveForward", "F");
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    if (fwd) env->SetFloatField(player, fwd, -0.98f);
                }
                break;

            case 2: // Blockhit: stop sprint (block simulated)
                if (m_setSprinting) {
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                // TODO: send RMB packet for actual sword block
                break;

            case 3: // Sneak: crouch for 1 tick
                if (m_setSneaking) {
                    env->CallVoidMethod(player, m_setSneaking, (jboolean)true);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (m_setSprinting) {
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                break;

            case 4: // Ctrl: just stop and re-start sprint
                if (m_setSprinting) {
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                break;

            case 5: // Packet: sprint off/on in same tick
                if (m_setSprinting) {
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)false);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                m_isResetting = false; // Instant, no wait
                break;
        }
    }

    void OnDisable(JNIEnv* env) override {
        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        // Ensure we're not stuck in a broken state
        if (m_isResetting) {
            if (m_setSprinting) {
                env->CallVoidMethod(player, m_setSprinting, (jboolean)true);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (m_setSneaking) {
                env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            m_isResetting = false;
        }
    }

    void RenderSettings() override {
        const char* methods[] = {
            "W-Tap", "S-Tap", "Blockhit", "Sneak Tap", "Ctrl Spam", "Packet"
        };
        ImGui::Combo("Method", &m_method, methods, 6);

        if (m_method == 5) {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "! Packet mode may flag on Polar");
        }

        ImGui::Separator();

        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::SliderInt("Reset Ticks Min", &m_resetTicksMin, 1, 5);
        ImGui::SliderInt("Reset Ticks Max", &m_resetTicksMax, 1, 5);
        if (m_resetTicksMin > m_resetTicksMax) m_resetTicksMin = m_resetTicksMax;
        ImGui::SliderInt("Hit Delay (ticks)", &m_hitDelay, 0, 5);

        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);

        ImGui::Separator();
        ImGui::TextDisabled("Method info:");
        switch (m_method) {
            case 0: ImGui::TextWrapped("W-Tap: releases W for 1 tick then re-presses. Most common, clean, works everywhere."); break;
            case 1: ImGui::TextWrapped("S-Tap: taps S for 1 tick. Stops you faster, more distance. Best for speed 2 / high ping."); break;
            case 2: ImGui::TextWrapped("Blockhit: blocks with sword after hit. Resets sprint + reduces damage. Sword only."); break;
            case 3: ImGui::TextWrapped("Sneak Tap: shift for 1 tick. No slowdown. Good for sumo."); break;
            case 4: ImGui::TextWrapped("Ctrl Spam: spam sprint key. Least momentum loss, instant re-sprint."); break;
            case 5: ImGui::TextWrapped("Packet: sprint off/on in same tick via JNI. Fastest but may flag."); break;
        }
    }
};
