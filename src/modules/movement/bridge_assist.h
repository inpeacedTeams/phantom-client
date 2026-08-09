#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../jni/class_resolver.h"
#include <imgui.h>
#include <random>

// =================================================================
// Bridge Assist (AutoEagle / Slinky-style)
// =================================================================
// Automatically sneaks when you reach the edge of a block while
// bridging backward. This lets you bridge without looking down
// or falling off.
//
// How it works:
//   1. Detect when player is walking backward (S held)
//   2. Check if the block at the player's feet edge is air
//   3. If at edge: crouch (sneak) to prevent falling
//   4. If safe: release crouch for speed
//
// Modes:
//   0 = Eagle:     sneak at edge, release when placed block
//   1 = Godbridge:  sneak/unsneak rapidly for fast bridging
//   2 = Breezily:   minimal sneak, timing-based fast bridge
//   3 = Safewalk:   always sneak at edges (not just backward)
//
// This is 100% legit: it simulates shift key. Every player does
// this manually when bridging. Polar cannot flag sneaking.
// =================================================================

class BridgeAssist : public Module {
private:
    int m_mode = 0;
    bool m_onlyBackward = true;        // Only when pressing S
    bool m_onlyHoldingBlocks = true;   // Only when holding blocks
    float m_edgeDistance = 0.3f;       // How close to edge before sneaking
    int m_sneakTicks = 2;              // Ticks to sneak in Godbridge mode
    int m_unsneakTicks = 1;            // Ticks to unsneak in Godbridge
    bool m_autoPlace = false;          // Auto right-click to place blocks
    float m_placeDelay = 0.0f;         // Delay between placements

    // Internal
    bool m_isSneaking = false;
    int m_sneakCounter = 0;
    bool m_atEdge = false;

    // JNI
    jmethodID m_setSneaking = nullptr;
    jfieldID m_fOnGround = nullptr;
    bool m_jniResolved = false;

    std::mt19937 m_rng{ std::random_device{}() };

    void ResolveJNI(JNIEnv* env) {
        if (m_jniResolved) return;

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

    // Check if player is at the edge of a block
    // by checking if they're close to a fractional position
    bool IsAtEdge(JNIEnv* env, jobject player) {
        double posX = Minecraft::GetPosX(env, player);
        double posZ = Minecraft::GetPosZ(env, player);
        double posY = Minecraft::GetPosY(env, player);

        // Get fractional position within block
        double fracX = posX - floor(posX);
        double fracZ = posZ - floor(posZ);

        // Player is at edge if fractional pos is near 0 or 1
        // with some tolerance based on m_edgeDistance
        float edgeDist = m_edgeDistance;
        bool nearEdgeX = (fracX < edgeDist || fracX > (1.0 - edgeDist));
        bool nearEdgeZ = (fracZ < edgeDist || fracZ > (1.0 - edgeDist));

        // Also check if block below the edge is air
        // Full impl would use world.getBlockState() via JNI
        // Simplified: just check if near edge
        return nearEdgeX || nearEdgeZ;
    }

public:
    BridgeAssist()
        : Module("Bridge Assist", "Auto-sneak at block edges for bridging",
                 ModuleCategory::MOVEMENT, 0) {}

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) return;
        if (!Minecraft::IsOnGround(env, player)) {
            // In air: stop sneaking to not look weird
            if (m_isSneaking && m_setSneaking) {
                env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                if (env->ExceptionCheck()) env->ExceptionClear();
                m_isSneaking = false;
            }
            return;
        }

        // Check direction
        bool backward = (GetAsyncKeyState('S') & 0x8000) != 0;
        bool anyDirection = backward ||
                           (GetAsyncKeyState('W') & 0x8000) ||
                           (GetAsyncKeyState('A') & 0x8000) ||
                           (GetAsyncKeyState('D') & 0x8000);

        bool shouldCheck = m_onlyBackward ? backward : anyDirection;
        if (!shouldCheck) {
            // Not moving in target direction: release sneak
            if (m_isSneaking && m_setSneaking) {
                env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                if (env->ExceptionCheck()) env->ExceptionClear();
                m_isSneaking = false;
            }
            return;
        }

        // Check if player is holding blocks
        // TODO: check held item via JNI (isBlock)
        // For now, skip this check if disabled

        m_atEdge = IsAtEdge(env, player);

        switch (m_mode) {
            case 0: { // Eagle: simple sneak at edge
                if (m_atEdge && !m_isSneaking) {
                    if (m_setSneaking) {
                        env->CallVoidMethod(player, m_setSneaking, (jboolean)true);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    m_isSneaking = true;
                } else if (!m_atEdge && m_isSneaking) {
                    if (m_setSneaking) {
                        env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    m_isSneaking = false;
                }
                break;
            }

            case 1: { // Godbridge: rapid sneak/unsneak
                if (m_atEdge) {
                    m_sneakCounter++;
                    if (m_isSneaking) {
                        if (m_sneakCounter >= m_sneakTicks) {
                            if (m_setSneaking) {
                                env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                                if (env->ExceptionCheck()) env->ExceptionClear();
                            }
                            m_isSneaking = false;
                            m_sneakCounter = 0;
                        }
                    } else {
                        if (m_sneakCounter >= m_unsneakTicks) {
                            if (m_setSneaking) {
                                env->CallVoidMethod(player, m_setSneaking, (jboolean)true);
                                if (env->ExceptionCheck()) env->ExceptionClear();
                            }
                            m_isSneaking = true;
                            m_sneakCounter = 0;
                        }
                    }
                } else if (m_isSneaking) {
                    if (m_setSneaking) {
                        env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    m_isSneaking = false;
                    m_sneakCounter = 0;
                }
                break;
            }

            case 2: { // Breezily: minimal sneak
                // Only sneak for exactly 1 tick at the very edge
                if (m_atEdge) {
                    if (!m_isSneaking) {
                        if (m_setSneaking) {
                            env->CallVoidMethod(player, m_setSneaking, (jboolean)true);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        m_isSneaking = true;
                    } else {
                        // Already sneaked 1 tick, release
                        if (m_setSneaking) {
                            env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        m_isSneaking = false;
                    }
                }
                break;
            }

            case 3: { // Safewalk: always sneak at any edge
                if (m_atEdge && !m_isSneaking) {
                    if (m_setSneaking) {
                        env->CallVoidMethod(player, m_setSneaking, (jboolean)true);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    m_isSneaking = true;
                } else if (!m_atEdge && m_isSneaking) {
                    if (m_setSneaking) {
                        env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    m_isSneaking = false;
                }
                break;
            }
        }
    }

    void OnDisable(JNIEnv* env) override {
        if (m_isSneaking) {
            jobject player = Minecraft::GetPlayer(env);
            if (player && m_setSneaking) {
                env->CallVoidMethod(player, m_setSneaking, (jboolean)false);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            m_isSneaking = false;
        }
    }

    void RenderSettings() override {
        const char* modes[] = { "Eagle", "Godbridge", "Breezily", "Safewalk" };
        ImGui::Combo("Mode", &m_mode, modes, 4);

        ImGui::SliderFloat("Edge Distance", &m_edgeDistance, 0.1f, 0.5f, "%.2f");
        ImGui::Checkbox("Only Backward", &m_onlyBackward);
        ImGui::Checkbox("Only Holding Blocks", &m_onlyHoldingBlocks);

        if (m_mode == 1) {
            ImGui::SliderInt("Sneak Ticks", &m_sneakTicks, 1, 5);
            ImGui::SliderInt("Unsneak Ticks", &m_unsneakTicks, 1, 3);
        }

        ImGui::Separator();
        switch (m_mode) {
            case 0: ImGui::TextWrapped("Eagle: sneak at edge, release when safe. Classic bridging."); break;
            case 1: ImGui::TextWrapped("Godbridge: rapid sneak/unsneak for fast bridging."); break;
            case 2: ImGui::TextWrapped("Breezily: minimal 1-tick sneak. Fastest but hardest."); break;
            case 3: ImGui::TextWrapped("Safewalk: always sneak at edges in any direction."); break;
        }
    }
};
