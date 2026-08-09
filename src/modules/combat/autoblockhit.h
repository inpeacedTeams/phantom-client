#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/entity_list.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <jvmti.h>
#include <random>

// =================================================================
// Auto Blockhit
// =================================================================
// Blocking with a sword in 1.8 halves incoming damage and resets
// sprint. Blocking a lot is not suspicious on its own: real
// blockhitters keep right click down for most of a fight. What
// gets flagged is HOW the block is produced.
//
//   BAD  block/unblock pairs with identical tick gaps
//   BAD  blocking without a sword in hand
//   BAD  toggling faster than the item-use cooldown allows
//   GOOD right click held, with human micro-releases
//
// We drive keyBindUseItem, so Minecraft emits the same packets a
// real right-click hold produces.
//
// PERFECT mode aims for near-total coverage, releasing only for the
// tick a swing needs plus randomized micro-gaps.
// =================================================================

class AutoBlockhit : public Module {
private:
    int   m_mode = 4;   // 0 Normal 1 Timed 2 Fake 3 Smart 4 Perfect

    float m_chance          = 100.0f;
    int   m_blockTicksMin   = 1;
    int   m_blockTicksMax   = 2;
    bool  m_onlySword       = true;
    bool  m_onlyWhileMoving = false;

    int   m_hitIntervalMin  = 1;
    int   m_hitIntervalMax  = 3;

    float m_coverage        = 94.0f;
    int   m_swingGapTicks   = 1;
    bool  m_microRelease    = true;
    float m_microChance     = 7.0f;
    int   m_microLenMin     = 1;
    int   m_microLenMax     = 2;
    bool  m_predictive      = true;
    float m_predictRange    = 3.8f;
    bool  m_holdBetweenHits = true;
    int   m_reblockDelayMin = 0;
    int   m_reblockDelayMax = 1;

    bool  m_varyReleaseTiming = true;
    float m_timingNoise     = 30.0f;
    bool  m_pauseOnFlag     = true;
    int   m_flagPauseTicks  = 20;

    // State
    bool m_blocking         = false;
    int  m_blockCountdown   = 0;
    int  m_unblockCountdown = 0;
    int  m_hitCounter       = 0;
    int  m_nextBlockAt      = 1;
    bool m_lastLMB          = false;
    int  m_flagPause        = 0;
    int  m_ticksBlocked     = 0;
    int  m_ticksTotal       = 0;

    // Sword check is walked once per second, not per tick
    bool m_swordCached      = true;
    int  m_swordCheckTimer  = 0;

    jfieldID  m_fInventory = nullptr;
    jmethodID m_mCurItem   = nullptr;
    jmethodID m_mGetItem   = nullptr;
    jclass    m_itemSword  = nullptr;
    bool      m_resolved   = false;

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
        float r = base * (m_timingNoise / 100.f);
        std::uniform_real_distribution<float> d(-r, r);
        int v = base + (int)d(m_rng);
        return v < 0 ? 0 : v;
    }

    // ItemSword is obfuscated, so find it by the field it declares
    jclass FindItemSword(JNIEnv* env) {
        jvmtiEnv* jvmti = JvmtiUtil::Get(env);
        if (!jvmti) return nullptr;

        jint count = 0;
        jclass* classes = nullptr;
        if (jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE) return nullptr;

        jclass found = nullptr;
        for (jint i = 0; i < count && !found; i++) {
            if (JvmtiUtil::HasField(env, classes[i], "field_150934_a")
             || JvmtiUtil::HasField(env, classes[i], "attackDamage")) {
                found = (jclass)env->NewGlobalRef(classes[i]);
            }
        }
        jvmti->Deallocate((unsigned char*)classes);
        return found;
    }

    void ResolveJNI(JNIEnv* env) {
        if (m_resolved) return;

        if (ClassResolver::entityPlayer) {
            m_fInventory = JvmtiUtil::FindField(env, ClassResolver::entityPlayer,
                { "field_71071_by", "inventory" });
        }
        if (m_fInventory) {
            jobject player = Minecraft::GetPlayer(env);
            if (player) {
                jobject inv = env->GetObjectField(player, m_fInventory);
                if (inv) {
                    jclass invCls = env->GetObjectClass(inv);
                    m_mCurItem = JvmtiUtil::FindMethod(env, invCls,
                        { "func_70448_g", "getCurrentItem" }, 0);
                    env->DeleteLocalRef(invCls);
                }
            }
        }
        if (!m_itemSword) m_itemSword = FindItemSword(env);

        m_resolved = true;
        printf("[AutoBlock] inv=%p curItem=%p sword=%p\n",
            (void*)m_fInventory, (void*)m_mCurItem, (void*)m_itemSword);
    }

    bool CheckSword(JNIEnv* env, jobject player) {
        if (!m_onlySword) return true;
        if (!m_fInventory || !m_mCurItem || !m_itemSword) return true;

        jobject inv = env->GetObjectField(player, m_fInventory);
        if (!inv) return false;

        jobject stack = env->CallObjectMethod(inv, m_mCurItem);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!stack) return false;

        if (!m_mGetItem) {
            jclass sc = env->GetObjectClass(stack);
            m_mGetItem = JvmtiUtil::FindMethod(env, sc, { "func_77973_b", "getItem" }, 0);
            env->DeleteLocalRef(sc);
        }
        if (!m_mGetItem) return false;

        jobject item = env->CallObjectMethod(stack, m_mGetItem);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!item) return false;

        return env->IsInstanceOf(item, m_itemSword) != 0;
    }

    void SetBlock(JNIEnv* env, bool on) {
        if (m_blocking == on) return;
        KeyBinds::SetUseItem(env, on);
        m_blocking = on;
    }

    bool EnemyInRange(JNIEnv* env) {
        if (!EntityList::Init(env)) return false;
        return !EntityList::GetPlayers(env, m_predictRange).empty();
    }

public:
    AutoBlockhit()
        : Module("Auto Blockhit", "Sword block with near-total coverage",
                 ModuleCategory::COMBAT, 0)
    {
        Bind("Mode", &m_mode);
        Bind("Chance", &m_chance);
        Bind("Block Ticks Min", &m_blockTicksMin);
        Bind("Block Ticks Max", &m_blockTicksMax);
        Bind("Only Sword", &m_onlySword);
        Bind("Only While Moving", &m_onlyWhileMoving);
        Bind("Hit Interval Min", &m_hitIntervalMin);
        Bind("Hit Interval Max", &m_hitIntervalMax);
        Bind("Coverage", &m_coverage);
        Bind("Swing Gap", &m_swingGapTicks);
        Bind("Micro Release", &m_microRelease);
        Bind("Micro Chance", &m_microChance);
        Bind("Micro Len Min", &m_microLenMin);
        Bind("Micro Len Max", &m_microLenMax);
        Bind("Predictive", &m_predictive);
        Bind("Predict Range", &m_predictRange);
        Bind("Hold Between Hits", &m_holdBetweenHits);
        Bind("Reblock Delay Min", &m_reblockDelayMin);
        Bind("Reblock Delay Max", &m_reblockDelayMax);
        Bind("Vary Release Timing", &m_varyReleaseTiming);
        Bind("Timing Noise", &m_timingNoise);
        Bind("Pause On Flag", &m_pauseOnFlag);
        Bind("Flag Pause Ticks", &m_flagPauseTicks);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;
        ResolveJNI(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        if (Minecraft::IsInGui(env)) { SetBlock(env, false); return; }

        if (m_flagPause > 0) {
            m_flagPause--;
            SetBlock(env, false);
            return;
        }

        if (m_onlyWhileMoving && !KeyBinds::GetForward(env)) {
            SetBlock(env, false);
            return;
        }

        // Walking the inventory every tick is wasteful, and the held
        // item rarely changes mid-swing.
        if (--m_swordCheckTimer <= 0) {
            m_swordCached = CheckSword(env, player);
            m_swordCheckTimer = 10;
        }
        if (!m_swordCached) { SetBlock(env, false); return; }

        m_ticksTotal++;
        if (m_blocking) m_ticksBlocked++;
        if (m_ticksTotal > 400) { m_ticksTotal /= 2; m_ticksBlocked /= 2; }

        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool justClicked = lmb && !m_lastLMB;
        m_lastLMB = lmb;

        // ---------------- PERFECT ----------------
        if (m_mode == 4) {
            if (justClicked) {
                SetBlock(env, false);
                m_unblockCountdown = Noisy(m_swingGapTicks);
                if (m_unblockCountdown < 1) m_unblockCountdown = 1;
                return;
            }
            if (m_unblockCountdown > 0) {
                if (--m_unblockCountdown == 0)
                    m_blockCountdown = Rand(m_reblockDelayMin, m_reblockDelayMax);
                return;
            }
            if (m_blockCountdown > 0) { m_blockCountdown--; return; }

            if (m_microRelease && m_blocking && Roll(m_microChance)) {
                float actual = m_ticksTotal > 0
                    ? (100.f * m_ticksBlocked / m_ticksTotal) : 0.f;
                if (actual > m_coverage) {
                    SetBlock(env, false);
                    m_unblockCountdown = Rand(m_microLenMin, m_microLenMax);
                    return;
                }
            }

            if (m_predictive && !m_blocking && EnemyInRange(env)) {
                SetBlock(env, true);
                return;
            }
            if (m_holdBetweenHits && !m_blocking && Roll(m_chance)) {
                SetBlock(env, true);
            }
            return;
        }

        // ---------------- LEGACY 0-3 ----------------
        if (m_blocking) {
            if (m_blockCountdown <= 0) SetBlock(env, false);
            else { m_blockCountdown--; return; }
        }

        if (!justClicked) return;
        m_hitCounter++;

        bool shouldBlock = false;
        switch (m_mode) {
            case 0: shouldBlock = true; break;
            case 1:
                if (m_hitCounter >= m_nextBlockAt) {
                    shouldBlock = true;
                    m_hitCounter = 0;
                    m_nextBlockAt = Rand(m_hitIntervalMin, m_hitIntervalMax);
                }
                break;
            case 2: shouldBlock = true; break;
            case 3: shouldBlock = EnemyInRange(env); break;
        }

        if (!shouldBlock || !Roll(m_chance)) return;

        m_blockCountdown = Noisy(Rand(m_blockTicksMin, m_blockTicksMax));
        if (m_mode != 2) SetBlock(env, true);   // Fake never sends the block
    }

    void OnDisable(JNIEnv* env) override {
        SetBlock(env, false);
        m_blockCountdown = 0;
        m_unblockCountdown = 0;
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
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.6f, 1.f), "Live coverage: %.1f%%", actual);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Coverage");
            ImGui::SliderFloat("Target Coverage", &m_coverage, 60.f, 99.f, "%.0f%%");
            ImGui::SliderInt("Swing Gap (ticks)", &m_swingGapTicks, 1, 3);
            ImGui::Checkbox("Hold Between Hits", &m_holdBetweenHits);
            ImGui::SliderInt("Reblock Delay Min", &m_reblockDelayMin, 0, 4);
            ImGui::SliderInt("Reblock Delay Max", &m_reblockDelayMax, 0, 5);
            if (m_reblockDelayMin > m_reblockDelayMax) m_reblockDelayMin = m_reblockDelayMax;

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
                if (m_hitIntervalMin > m_hitIntervalMax) m_hitIntervalMin = m_hitIntervalMax;
            }
            if (m_mode == 3)
                ImGui::SliderFloat("Predict Range", &m_predictRange, 2.f, 6.f, "%.1f");
        }

        ImGui::Separator();
        ImGui::Checkbox("Only Sword", &m_onlySword);
        ImGui::Checkbox("Only While Moving", &m_onlyWhileMoving);
        ImGui::Checkbox("Pause On Flag", &m_pauseOnFlag);
        if (m_pauseOnFlag) ImGui::SliderInt("Flag Pause Ticks", &m_flagPauseTicks, 5, 60);

        if (m_onlySword && !m_itemSword) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "ItemSword unresolved: sword check skipped");
        }
        if (!KeyBinds::HasUseItem()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Use-item keybind unresolved: module inactive");
        }
    }
};
