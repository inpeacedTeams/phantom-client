#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/entity_list.h"
#include "../../jni/class_resolver.h"
#include <imgui.h>
#include <random>

// =================================================================
// Auto Blockhit
// =================================================================
// Blocking with a sword in 1.8:
//   - halves incoming damage
//   - resets sprint (more KB on your next hit)
//   - is a completely vanilla action
//
// Blocking a LOT is not itself bannable. Real blockhitters keep
// RMB down nearly the whole fight. What gets flagged is HOW the
// block is produced:
//
//   BAD  -> block/unblock packet pairs with identical tick gaps
//   BAD  -> blocking with a non-sword item
//   BAD  -> block release landing on the exact same tick as swing
//   BAD  -> block state toggling faster than the item-use cooldown
//   GOOD -> RMB held down with occasional human micro-releases
//
// PERFECT mode targets near-total coverage: it holds the block
// continuously and only releases for the 1-tick windows required
// to swing, plus randomized micro-gaps so the packet stream keeps
// human-like variance.
// =================================================================

class AutoBlockhit : public Module {
private:
    // 0=Normal 1=Timed 2=Fake 3=Smart 4=Perfect
    int   m_mode = 4;

    // ---- Shared ----
    float m_chance          = 100.0f;
    int   m_blockTicksMin   = 1;
    int   m_blockTicksMax   = 2;
    bool  m_onlySword       = true;
    bool  m_onlyWhileMoving = false;  // Off: blocking while still is normal

    // ---- Timed mode ----
    int   m_hitIntervalMin  = 1;
    int   m_hitIntervalMax  = 3;

    // ---- Perfect mode ----
    float m_coverage        = 94.0f;  // Target % of ticks spent blocking
    int   m_swingGapTicks   = 1;      // Ticks unblocked to let the swing land
    bool  m_microRelease    = true;   // Occasional extra 1-tick gaps
    float m_microChance     = 7.0f;   // Chance per tick of a micro-gap
    int   m_microLenMin     = 1;
    int   m_microLenMax     = 2;
    bool  m_predictive      = true;   // Block before the enemy swing lands
    float m_predictRange    = 3.8f;   // Enemy within this = pre-block
    bool  m_holdBetweenHits = true;   // Stay blocked while not swinging
    int   m_reblockDelayMin = 0;      // Ticks before re-blocking after swing
    int   m_reblockDelayMax = 1;

    // ---- Anti-pattern ----
    bool  m_varyReleaseTiming = true; // Never release on a fixed cadence
    float m_timingNoise     = 30.0f;  // % variance on all block durations
    bool  m_pauseOnFlag     = true;
    int   m_flagPauseTicks  = 20;

    // ---- Internal ----
    bool  m_isBlocking      = false;
    int   m_blockCountdown  = 0;
    int   m_unblockCountdown = 0;
    int   m_hitCounter      = 0;
    int   m_nextBlockAt     = 1;
    bool  m_lastLMB         = false;
    int   m_flagPause       = 0;
    int   m_ticksBlocked    = 0;
    int   m_ticksTotal      = 0;

    // ---- JNI ----
    jobject m_keyBindUseItem = nullptr;  // GameSettings.keyBindUseItem
    jfieldID m_fPressed      = nullptr;  // KeyBinding.pressed
    jmethodID m_mHeldItem    = nullptr;  // EntityPlayer.getHeldItem()
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

    int Noisy(int base) {
        if (!m_varyReleaseTiming || m_timingNoise <= 0.f) return base;
        float range = base * (m_timingNoise / 100.f);
        std::uniform_real_distribution<float> d(-range, range);
        int v = base + (int)d(m_rng);
        return v < 0 ? 0 : v;
    }

    void ResolveJNI(JNIEnv* env) {
        if (m_jniResolved) return;

        // Minecraft.gameSettings -> GameSettings.keyBindUseItem -> KeyBinding.pressed
        //
        // Driving keyBindUseItem.pressed is the cleanest route: the game
        // then emits exactly the same packet sequence a real RMB hold does.
        // No synthetic C08 spam, no timing that the item-use cooldown
        // would never allow.
        jobject mc = Minecraft::GetInstance(env);
        if (mc && ClassResolver::mcClass && ClassResolver::gameSettings) {
            jfieldID fGs = env->GetFieldID(ClassResolver::mcClass,
                "field_71474_y", "Ljava/lang/Object;");
            if (env->ExceptionCheck()) { env->ExceptionClear();
                fGs = env->GetFieldID(ClassResolver::mcClass,
                    "gameSettings", "Ljava/lang/Object;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); fGs = nullptr; }
            }

            if (fGs) {
                jobject gs = env->GetObjectField(mc, fGs);
                if (gs) {
                    jfieldID fKey = env->GetFieldID(ClassResolver::gameSettings,
                        "field_74313_G", "Ljava/lang/Object;"); // keyBindUseItem
                    if (env->ExceptionCheck()) { env->ExceptionClear();
                        fKey = env->GetFieldID(ClassResolver::gameSettings,
                            "keyBindUseItem", "Ljava/lang/Object;");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); fKey = nullptr; }
                    }
                    if (fKey) {
                        jobject kb = env->GetObjectField(gs, fKey);
                        if (kb) {
                            m_keyBindUseItem = env->NewGlobalRef(kb);
                            jclass kbCls = env->GetObjectClass(kb);
                            m_fPressed = env->GetFieldID(kbCls, "field_74513_e", "Z");
                            if (env->ExceptionCheck()) { env->ExceptionClear();
                                m_fPressed = env->GetFieldID(kbCls, "pressed", "Z");
                                if (env->ExceptionCheck()) env->ExceptionClear();
                            }
                        }
                    }
                }
            }
        }

        m_jniResolved = true;
    }

    void SetBlock(JNIEnv* env, bool on) {
        if (m_keyBindUseItem && m_fPressed) {
            env->SetBooleanField(m_keyBindUseItem, m_fPressed, (jboolean)on);
        }
        m_isBlocking = on;
    }

    // Enemy is winding up: their swingProgress just started
    bool EnemyAboutToHit(JNIEnv* env) {
        EntityList::Init(env);
        auto ents = EntityList::GetPlayers(env, m_predictRange);
        for (auto& e : ents) {
            if (e.distanceToPlayer <= m_predictRange) return true;
        }
        return false;
    }

public:
    AutoBlockhit()
        : Module("Auto Blockhit", "Sword block with near-total coverage",
                 ModuleCategory::COMBAT, 0) {}

    void OnTick(JNIEnv* env) override {
        ResolveJNI(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;
        if (Minecraft::IsInGui(env)) {
            if (m_isBlocking) SetBlock(env, false);
            return;
        }

        if (m_flagPause > 0) {
            m_flagPause--;
            if (m_isBlocking) SetBlock(env, false);
            return;
        }

        if (m_onlyWhileMoving && !(GetAsyncKeyState('W') & 0x8000)) {
            if (m_isBlocking) SetBlock(env, false);
            return;
        }

        m_ticksTotal++;
        if (m_isBlocking) m_ticksBlocked++;
        if (m_ticksTotal > 400) { m_ticksTotal /= 2; m_ticksBlocked /= 2; }

        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool justClicked = lmb && !m_lastLMB;
        m_lastLMB = lmb;

        // =========================================================
        // PERFECT MODE
        // =========================================================
        if (m_mode == 4) {
            // A swing is queued: drop the block for the swing window
            if (justClicked) {
                SetBlock(env, false);
                m_unblockCountdown = Noisy(m_swingGapTicks);
                if (m_unblockCountdown < 1) m_unblockCountdown = 1;
                return;
            }

            // Waiting out the swing gap
            if (m_unblockCountdown > 0) {
                m_unblockCountdown--;
                if (m_unblockCountdown == 0) {
                    m_blockCountdown = Rand(m_reblockDelayMin, m_reblockDelayMax);
                }
                return;
            }

            // Waiting out the re-block delay
            if (m_blockCountdown > 0) {
                m_blockCountdown--;
                return;
            }

            // Micro-release: keeps the packet stream from looking
            // like a perfectly held key across a whole fight
            if (m_microRelease && m_isBlocking && Roll(m_microChance)) {
                float actual = m_ticksTotal > 0
                    ? (100.f * m_ticksBlocked / m_ticksTotal) : 0.f;
                if (actual > m_coverage) {   // Only if we're above target
                    SetBlock(env, false);
                    m_unblockCountdown = Rand(m_microLenMin, m_microLenMax);
                    return;
                }
            }

            // Predictive: pre-block when an enemy is inside reach
            if (m_predictive && !m_isBlocking) {
                if (EnemyAboutToHit(env)) { SetBlock(env, true); return; }
            }

            // Default: hold the block
            if (m_holdBetweenHits && !m_isBlocking) {
                if (Roll(m_chance)) SetBlock(env, true);
            }
            return;
        }

        // =========================================================
        // LEGACY MODES (0-3)
        // =========================================================
        if (m_isBlocking) {
            if (m_blockCountdown <= 0) SetBlock(env, false);
            else { m_blockCountdown--; return; }
        }

        if (!justClicked) return;
        m_hitCounter++;

        bool shouldBlock = false;
        switch (m_mode) {
            case 0:
                shouldBlock = true;
                break;
            case 1:
                if (m_hitCounter >= m_nextBlockAt) {
                    shouldBlock = true;
                    m_hitCounter = 0;
                    m_nextBlockAt = Rand(m_hitIntervalMin, m_hitIntervalMax);
                }
                break;
            case 2:
                shouldBlock = true;   // Client-side animation only
                break;
            case 3:
                shouldBlock = EnemyAboutToHit(env);
                break;
        }

        if (!shouldBlock || !Roll(m_chance)) return;

        m_blockCountdown = Noisy(Rand(m_blockTicksMin, m_blockTicksMax));
        if (m_mode != 2) SetBlock(env, true);
        else m_isBlocking = true;     // Fake: no real packet
    }

    void OnDisable(JNIEnv* env) override {
        if (m_isBlocking) SetBlock(env, false);
        m_blockCountdown = m_unblockCountdown = 0;
    }

    void OnServerCorrection() {
        if (m_pauseOnFlag) m_flagPause = m_flagPauseTicks;
    }

    void RenderSettings() override {
        const char* modes[] = { "Normal", "Timed", "Fake", "Smart", "Perfect" };
        ImGui::Combo("Mode", &m_mode, modes, 5);

        if (m_mode == 4) {
            float actual = m_ticksTotal > 0
                ? (100.f * m_ticksBlocked / m_ticksTotal) : 0.f;
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f),
                "Live coverage: %.1f%%", actual);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Coverage");
            ImGui::SliderFloat("Target Coverage", &m_coverage, 60.f, 99.f, "%.0f%%");
            ImGui::SliderInt("Swing Gap (ticks)", &m_swingGapTicks, 1, 3);
            ImGui::Checkbox("Hold Between Hits", &m_holdBetweenHits);
            ImGui::SliderInt("Reblock Delay Min", &m_reblockDelayMin, 0, 4);
            ImGui::SliderInt("Reblock Delay Max", &m_reblockDelayMax, 0, 5);
            if (m_reblockDelayMin > m_reblockDelayMax)
                m_reblockDelayMin = m_reblockDelayMax;

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Humanization");
            ImGui::Checkbox("Micro Release", &m_microRelease);
            if (m_microRelease) {
                ImGui::SliderFloat("Micro Chance", &m_microChance, 1.f, 25.f, "%.0f%%");
                ImGui::SliderInt("Micro Len Min", &m_microLenMin, 1, 4);
                ImGui::SliderInt("Micro Len Max", &m_microLenMax, 1, 5);
                if (m_microLenMin > m_microLenMax) m_microLenMin = m_microLenMax;
            }
            ImGui::Checkbox("Vary Release Timing", &m_varyReleaseTiming);
            if (m_varyReleaseTiming)
                ImGui::SliderFloat("Timing Noise", &m_timingNoise, 0.f, 60.f, "%.0f%%");

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Predictive");
            ImGui::Checkbox("Pre-Block On Approach", &m_predictive);
            if (m_predictive)
                ImGui::SliderFloat("Predict Range", &m_predictRange, 2.f, 6.f, "%.1f");

        } else {
            ImGui::SliderFloat("Chance", &m_chance, 10.f, 100.f, "%.0f%%");
            ImGui::SliderInt("Block Ticks Min", &m_blockTicksMin, 1, 5);
            ImGui::SliderInt("Block Ticks Max", &m_blockTicksMax, 1, 6);
            if (m_blockTicksMin > m_blockTicksMax) m_blockTicksMin = m_blockTicksMax;
            if (m_mode == 1) {
                ImGui::SliderInt("Hit Interval Min", &m_hitIntervalMin, 1, 5);
                ImGui::SliderInt("Hit Interval Max", &m_hitIntervalMax, 1, 6);
                if (m_hitIntervalMin > m_hitIntervalMax)
                    m_hitIntervalMin = m_hitIntervalMax;
            }
            if (m_mode == 3)
                ImGui::SliderFloat("Predict Range", &m_predictRange, 2.f, 6.f, "%.1f");
        }

        ImGui::Separator();
        ImGui::Checkbox("Only Sword", &m_onlySword);
        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);
        ImGui::Checkbox("Pause On Flag", &m_pauseOnFlag);
        if (m_pauseOnFlag)
            ImGui::SliderInt("Flag Pause Ticks", &m_flagPauseTicks, 5, 60);
    }
};
