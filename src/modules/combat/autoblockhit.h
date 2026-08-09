#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include <imgui.h>
#include <random>

// =================================================================
// Auto Block Hit
// =================================================================
// Automatically blocks with sword after each hit you deal.
// In 1.8, blocking right after hitting:
//   1. Reduces incoming damage by 50%
//   2. Resets sprint (= more KB on next hit)
//   3. Looks legit (top players blockhit constantly)
//
// Implementation: after LMB click, send RMB (useItem) for N ticks,
// then release. Timing is randomized for legit appearance.
//
// Modes:
//   0 = Normal:   block after every hit for N ticks
//   1 = Timed:    block only when you time it well (every N hits)
//   2 = Fake:     only send block animation client-side (visual)
//   3 = Smart:    only block when enemy is about to hit you
// =================================================================

class AutoBlockhit : public Module {
private:
    int m_mode = 0;
    float m_chance = 85.0f;
    int m_blockTicksMin = 1;          // Min ticks to hold block
    int m_blockTicksMax = 2;          // Max ticks
    int m_hitIntervalMin = 1;         // Block every N hits (min)
    int m_hitIntervalMax = 1;         // Block every N hits (max)
    bool m_onlySword = true;          // Only when holding sword
    bool m_onlyWhileMoving = true;
    bool m_renderBlockAnimation = true; // Show arm block visually

    // Internal
    bool m_isBlocking = false;
    int m_blockCountdown = 0;
    int m_hitCounter = 0;
    int m_nextBlockAt = 1;            // Block on this hit number
    bool m_lastLMB = false;

    // JNI
    jmethodID m_setItemInUse = nullptr;  // For blocking
    jmethodID m_stopUsingItem = nullptr;
    bool m_jniResolved = false;

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        std::uniform_real_distribution<float> d(0.f, 100.f);
        return d(m_rng) < pct;
    }

    int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        std::uniform_int_distribution<int> d(lo, hi);
        return d(m_rng);
    }

    void ResolveJNI(JNIEnv* env) {
        if (m_jniResolved) return;

        // EntityLivingBase.setItemInUse(ItemStack, int)
        // In 1.8 blocking is done via sendQueue packet:
        //   C08PacketPlayerBlockPlacement
        // But the simplest legit way is to call:
        //   playerController.sendUseItem(player, world, itemStack)
        // Or directly manipulate the isBlocking state.
        //
        // For now we use KeyBinding simulation approach:
        // We call Minecraft.gameSettings.keyBindUseItem.pressed = true/false
        // This is the most legit method as it simulates actual RMB.

        m_jniResolved = true;
    }

public:
    AutoBlockhit()
        : Module("Auto Blockhit", "Auto-block with sword after hitting",
                 ModuleCategory::COMBAT, 0) {}

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;

        // Only while moving forward
        if (m_onlyWhileMoving && !(GetAsyncKeyState('W') & 0x8000))
            return;

        // =====================
        // Phase 2: Unblock
        // =====================
        if (m_isBlocking) {
            if (m_blockCountdown <= 0) {
                // Release RMB (stop blocking)
                // Simulate key release via sending key state
                // In real impl: set keyBindUseItem.pressed = false
                // or send C07PacketPlayerDigging RELEASE_USE_ITEM
                m_isBlocking = false;
            } else {
                m_blockCountdown--;
                // Keep RMB held (keep blocking)
                // keyBindUseItem.pressed = true
                return;
            }
        }

        // =====================
        // Phase 1: Detect hit and start block
        // =====================
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool justClicked = lmb && !m_lastLMB;
        m_lastLMB = lmb;

        if (!justClicked) return;

        m_hitCounter++;

        // Check if we should block on this hit
        bool shouldBlock = false;

        switch (m_mode) {
            case 0: // Normal: every hit
                shouldBlock = true;
                break;

            case 1: // Timed: every N hits
                if (m_hitCounter >= m_nextBlockAt) {
                    shouldBlock = true;
                    m_hitCounter = 0;
                    m_nextBlockAt = Rand(m_hitIntervalMin, m_hitIntervalMax);
                }
                break;

            case 2: // Fake: client-side only
                shouldBlock = true;
                // In real impl: only send swing animation, no actual block packet
                break;

            case 3: { // Smart: block when enemy is close / about to hit
                // Check if any nearby entity has low hurtTime
                // (meaning they recently attacked and might hit us)
                shouldBlock = true; // Simplified for now
                break;
            }
        }

        if (!shouldBlock) return;
        if (!Roll(m_chance)) return;

        // Start blocking
        m_isBlocking = true;
        m_blockCountdown = Rand(m_blockTicksMin, m_blockTicksMax);

        // Simulate RMB press (block with sword)
        // Implementation options:
        //
        // Option A (cleanest, most legit):
        //   Set keyBindUseItem.pressed = true via JNI
        //   This makes the game think RMB is held
        //
        // Option B (packet-based):
        //   Send C08PacketPlayerBlockPlacement directly
        //   Faster but may look different to AC
        //
        // Option C (SendInput):
        //   Simulate actual RMB via Windows input API
        //   Most legit but has input lag
        //
        // For Polar safety, Option A is recommended.
        // TODO: implement the actual JNI call to set keyBindUseItem
    }

    void OnDisable(JNIEnv* env) override {
        if (m_isBlocking) {
            // Make sure we stop blocking
            m_isBlocking = false;
        }
    }

    void RenderSettings() override {
        const char* modes[] = { "Normal", "Timed", "Fake", "Smart" };
        ImGui::Combo("Mode", &m_mode, modes, 4);

        ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
        ImGui::SliderInt("Block Ticks Min", &m_blockTicksMin, 1, 5);
        ImGui::SliderInt("Block Ticks Max", &m_blockTicksMax, 1, 5);
        if (m_blockTicksMin > m_blockTicksMax) m_blockTicksMin = m_blockTicksMax;

        if (m_mode == 1) {
            ImGui::SliderInt("Hit Interval Min", &m_hitIntervalMin, 1, 5);
            ImGui::SliderInt("Hit Interval Max", &m_hitIntervalMax, 1, 5);
            if (m_hitIntervalMin > m_hitIntervalMax) m_hitIntervalMin = m_hitIntervalMax;
        }

        ImGui::Checkbox("Only Sword", &m_onlySword);
        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);

        ImGui::Separator();
        ImGui::TextDisabled("Blocks with sword after each hit.");
        ImGui::TextDisabled("Reduces damage 50%% + resets sprint.");
    }
};
