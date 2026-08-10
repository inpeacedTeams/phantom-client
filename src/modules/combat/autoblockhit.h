#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/entity_list.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <imgui.h>
#include <Windows.h>
#include <unordered_map>
#include <deque>
#include <chrono>
#include <random>
#include <cstring>
#include <cctype>

// =================================================================
// Auto Blockhit
// =================================================================
// Blocking with a sword in 1.8 halves incoming damage and resets
// sprint. The old version simply held the block whenever the module
// was on, which is both useless and obvious: it blocked in an empty
// lobby, it blocked with a pickaxe, and it never let go in time to
// actually swing.
//
// This one watches the enemy and blocks around THEIR swing.
//
// HOW THE PREDICTION WORKS
// EntityLivingBase raises isSwingInProgress the moment a player
// swings, and the client sees that through the animation packet.
// Timing those rising edges gives their swing rhythm, usually a
// fairly steady 100-250ms for someone clicking normally.
//
// From that we know roughly when the next swing lands, so the block
// goes down a configurable lead time before it and comes back up
// once the hit has passed. If they stop swinging, the rhythm goes
// stale and we stop blocking entirely, which is what keeps the
// module quiet when nobody is fighting you.
//
// WHY THE SWORD CHECK CHANGED
// Hunting for the ItemSword class by field name was fragile and
// matched the wrong class often enough to block with a pickaxe.
// Unlocalised names are plain strings in the source and survive
// every obfuscation pass, so "item.swordDiamond" is a far more
// reliable signal than any field lookup.
// =================================================================

class AutoBlockhit : public Module {
private:
    using Clock = std::chrono::steady_clock;
    using Ms    = std::chrono::milliseconds;

    static long long MsSince(Clock::time_point t) {
        return std::chrono::duration_cast<Ms>(Clock::now() - t).count();
    }

    struct Enemy {
        bool  wasSwinging = false;
        bool  hasSwung    = false;
        Clock::time_point lastSwing;
        std::deque<long long> intervals;
        long long avgInterval = 0;
        unsigned long long lastSeen = 0;
    };

    // ---- Mode ----
    // 0 Predict, 1 Reactive, 2 In Range, 3 Swing Only
    int m_mode = 0;

    // ---- Detection ----
    float m_blockRange  = 3.7f;   // they can reach us from here
    float m_detectRange = 6.0f;   // start watching their rhythm

    // ---- Prediction ----
    int m_leadMs        = 130;    // go down this long before the swing
    int m_defaultCycle  = 220;    // assumed rhythm until one is learned
    int m_staleMs       = 1400;   // silence for this long means disengaged
    int m_reactionMin   = 35;     // nobody reacts instantly
    int m_reactionMax   = 95;

    // ---- Release ----
    int m_swingGap      = 1;      // ticks unblocked so our swing lands
    int m_afterHit      = 2;      // ticks unblocked once their hit passed
    int m_maxBlockTicks = 40;     // never hold longer than this

    // ---- Humanisation ----
    float m_chance      = 95.0f;
    bool  m_varyTiming  = true;
    float m_timingNoise = 25.0f;

    // ---- Safety ----
    bool m_onlySword     = true;
    bool m_pauseOnFlag   = true;
    int  m_flagPauseTicks = 20;

    // ---- State ----
    std::unordered_map<int, Enemy> m_enemies;
    bool m_blocking       = false;
    int  m_releaseLeft    = 0;    // ticks we must stay unblocked
    int  m_blockHeldTicks = 0;
    int  m_flagPause      = 0;
    bool m_lastLMB        = false;
    int  m_lastHurtTime   = 0;
    unsigned long long m_tick = 0;

    bool m_swordCached     = false;
    int  m_swordCheckTimer = 0;

    // Live readout
    long long m_lastPredictMs = -1;
    long long m_shownCycle    = 0;
    int  m_ticksBlocked = 0;
    int  m_ticksTotal   = 0;

    // ---- JNI ----
    jfieldID  m_fInventory   = nullptr;
    jmethodID m_mCurItem     = nullptr;
    jmethodID m_mStackName   = nullptr;   // ItemStack.getUnlocalizedName
    jfieldID  m_fSwinging    = nullptr;   // EntityLivingBase.isSwingInProgress
    jfieldID  m_fSwingProg   = nullptr;   // EntityLivingBase.swingProgress
    jmethodID m_getEntityId  = nullptr;
    bool m_resolved = false;

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
    long long Noisy(long long base) {
        if (!m_varyTiming || m_timingNoise <= 0.f) return base;
        float r = (float)base * (m_timingNoise / 100.f);
        std::uniform_real_distribution<float> d(-r, r);
        long long v = base + (long long)d(m_rng);
        return v < 0 ? 0 : v;
    }

    void Resolve(JNIEnv* env) {
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

        if (ClassResolver::entityLivingBase) {
            auto lb = ClassResolver::entityLivingBase;
            m_fSwinging  = JvmtiUtil::FindField(env, lb,
                { "field_82175_bq", "isSwingInProgress" });
            m_fSwingProg = JvmtiUtil::FindField(env, lb,
                { "field_70733_aJ", "swingProgress" });
        }

        if (ClassResolver::entity) {
            m_getEntityId = JvmtiUtil::FindMethod(env, ClassResolver::entity,
                { "func_145782_y", "getEntityId" }, 0);
        }

        m_resolved = true;
        printf("[AutoBlock] inv=%p item=%p swing=%p prog=%p id=%p\n",
            (void*)m_fInventory, (void*)m_mCurItem,
            (void*)m_fSwinging, (void*)m_fSwingProg, (void*)m_getEntityId);
    }

    // Unlocalised names are literal strings in the source, so they
    // read the same no matter how the classes were renamed.
    bool CheckSword(JNIEnv* env, jobject player) {
        if (!m_onlySword) return true;
        if (!m_fInventory || !m_mCurItem) return true;   // cannot tell, allow

        jobject inv = env->GetObjectField(player, m_fInventory);
        if (!inv) return false;

        jobject stack = env->CallObjectMethod(inv, m_mCurItem);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!stack) return false;                        // empty hand

        if (!m_mStackName) {
            jclass sc = env->GetObjectClass(stack);
            m_mStackName = JvmtiUtil::FindMethod(env, sc,
                { "func_77977_a", "getUnlocalizedName" }, 0);
            env->DeleteLocalRef(sc);
        }
        if (!m_mStackName) return true;                  // cannot tell, allow

        jstring js = (jstring)env->CallObjectMethod(stack, m_mStackName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!js) return false;

        const char* raw = env->GetStringUTFChars(js, nullptr);
        bool isSword = false;
        if (raw) {
            char low[128];
            size_t n = std::strlen(raw);
            if (n > sizeof(low) - 1) n = sizeof(low) - 1;
            for (size_t i = 0; i < n; i++)
                low[i] = (char)std::tolower((unsigned char)raw[i]);
            low[n] = '\0';
            isSword = (std::strstr(low, "sword") != nullptr);
            env->ReleaseStringUTFChars(js, raw);
        }
        env->DeleteLocalRef(js);
        return isSword;
    }

    bool IsSwinging(JNIEnv* env, jobject ent) {
        if (m_fSwinging) return env->GetBooleanField(ent, m_fSwinging) != 0;
        if (m_fSwingProg) return env->GetFloatField(ent, m_fSwingProg) > 0.001f;
        return false;
    }

    void SetBlock(JNIEnv* env, bool on) {
        if (m_blocking == on) return;
        KeyBinds::SetUseItem(env, on);
        m_blocking = on;
        if (!on) m_blockHeldTicks = 0;
    }

public:
    AutoBlockhit()
        : Module("Auto Blockhit", "Block around the enemy's swing, not always",
                 ModuleCategory::COMBAT, 0)
    {
        Bind("Mode", &m_mode);
        Bind("Block Range", &m_blockRange);
        Bind("Detect Range", &m_detectRange);
        Bind("Lead", &m_leadMs);
        Bind("Default Cycle", &m_defaultCycle);
        Bind("Stale", &m_staleMs);
        Bind("Reaction Min", &m_reactionMin);
        Bind("Reaction Max", &m_reactionMax);
        Bind("Swing Gap", &m_swingGap);
        Bind("After Hit", &m_afterHit);
        Bind("Max Block Ticks", &m_maxBlockTicks);
        Bind("Chance", &m_chance);
        Bind("Vary Timing", &m_varyTiming);
        Bind("Timing Noise", &m_timingNoise);
        Bind("Only Sword", &m_onlySword);
        Bind("Pause On Flag", &m_pauseOnFlag);
        Bind("Flag Pause Ticks", &m_flagPauseTicks);
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;
        Resolve(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;

        m_tick++;

        if (Minecraft::IsInGui(env)) { SetBlock(env, false); return; }

        if (m_flagPause > 0) {
            m_flagPause--;
            SetBlock(env, false);
            return;
        }

        // Walking the inventory every tick is wasteful and the held
        // item rarely changes mid-swing.
        if (--m_swordCheckTimer <= 0) {
            m_swordCached = CheckSword(env, player);
            m_swordCheckTimer = 6;
        }
        if (!m_swordCached) { SetBlock(env, false); return; }

        m_ticksTotal++;
        if (m_blocking) m_ticksBlocked++;
        if (m_ticksTotal > 400) { m_ticksTotal /= 2; m_ticksBlocked /= 2; }

        // ---- Our own swing frees the block ----
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool weSwung = lmb && !m_lastLMB;
        m_lastLMB = lmb;
        if (weSwung) {
            SetBlock(env, false);
            m_releaseLeft = m_swingGap;
        }

        // ---- Their hit landed, so the block did its job ----
        int hurt = Minecraft::GetHurtTime(env, player);
        if (hurt > 0 && m_lastHurtTime == 0) {
            SetBlock(env, false);
            m_releaseLeft = m_afterHit;
        }
        m_lastHurtTime = hurt;

        // ---- Watch every nearby enemy ----
        if (!EntityList::Init(env)) return;
        auto ents = EntityList::GetPlayers(env, m_detectRange);

        if (ents.empty()) {
            // Nobody near: this is the case the old build got wrong
            SetBlock(env, false);
            m_lastPredictMs = -1;
            m_shownCycle = 0;
            return;
        }

        bool  anySwingingNow = false;
        bool  anyInBlockRange = false;
        long long soonest = -1;

        for (auto& e : ents) {
            if (!m_getEntityId) break;
            jint id = env->CallIntMethod(e.ref, m_getEntityId);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

            Enemy& en = m_enemies[(int)id];
            en.lastSeen = m_tick;

            bool swinging = IsSwinging(env, e.ref);

            // Rising edge is the swing itself; holding the button
            // keeps the flag up for several ticks after.
            if (swinging && !en.wasSwinging) {
                if (en.hasSwung) {
                    long long gap = MsSince(en.lastSwing);
                    // Ignore nonsense: double events, or a pause so
                    // long it says nothing about their rhythm.
                    if (gap >= 60 && gap <= 1200) {
                        en.intervals.push_back(gap);
                        if (en.intervals.size() > 8) en.intervals.pop_front();

                        long long sum = 0;
                        for (auto v : en.intervals) sum += v;
                        en.avgInterval = sum / (long long)en.intervals.size();
                    }
                }
                en.lastSwing = Clock::now();
                en.hasSwung = true;
            }
            en.wasSwinging = swinging;

            bool inRange = (e.distanceToPlayer <= m_blockRange);
            if (inRange && swinging) anySwingingNow = true;
            if (inRange) anyInBlockRange = true;

            if (!inRange || !en.hasSwung) continue;

            long long since = MsSince(en.lastSwing);

            // Gone quiet: they have stopped attacking, so stop
            // guessing at a rhythm that no longer exists.
            if (since > m_staleMs) continue;

            long long cycle = en.avgInterval > 0 ? en.avgInterval : m_defaultCycle;
            long long until = cycle - (since % cycle);

            if (soonest < 0 || until < soonest) {
                soonest = until;
                m_shownCycle = cycle;
            }
        }

        // Forget anyone who walked away
        for (auto it = m_enemies.begin(); it != m_enemies.end(); ) {
            if (m_tick - it->second.lastSeen > 60) it = m_enemies.erase(it);
            else ++it;
        }

        m_lastPredictMs = soonest;

        // ---- Still inside a forced release ----
        if (m_releaseLeft > 0) {
            m_releaseLeft--;
            SetBlock(env, false);
            return;
        }

        // ---- Decide ----
        bool want = false;

        switch (m_mode) {
            case 1:   // Reactive: only while they are actually swinging
                want = anySwingingNow;
                break;

            case 2:   // In Range: block whenever someone can reach us
                want = anyInBlockRange;
                break;

            case 3:   // Swing Only: purely a sprint-reset aid
                want = false;
                break;

            default: { // Predict
                if (anySwingingNow) {
                    want = true;         // already mid-swing, block now
                } else if (soonest >= 0) {
                    long long lead = Noisy(m_leadMs)
                                   + Rand(m_reactionMin, m_reactionMax);
                    want = (soonest <= lead);
                }
                break;
            }
        }

        // Holding forever is both a tell and pointless
        if (want && m_blocking && m_blockHeldTicks >= m_maxBlockTicks) {
            SetBlock(env, false);
            m_releaseLeft = 2;
            return;
        }

        if (want && !m_blocking) {
            if (!Roll(m_chance)) return;   // occasionally miss one
            SetBlock(env, true);
        } else if (!want && m_blocking) {
            SetBlock(env, false);
        }

        if (m_blocking) m_blockHeldTicks++;
    }

    void OnDisable(JNIEnv* env) override {
        SetBlock(env, false);
        m_releaseLeft = 0;
        m_blockHeldTicks = 0;
        m_enemies.clear();
        m_lastPredictMs = -1;
    }

    void OnServerCorrection() {
        if (m_pauseOnFlag) m_flagPause = m_flagPauseTicks;
    }

    void RenderSettings() override {
        const char* modes[] = { "Predict", "Reactive", "In Range", "Swing Only" };
        ImGui::Combo("Mode", &m_mode, modes, 4);

        switch (m_mode) {
            case 0: ImGui::TextDisabled("Learns their swing rhythm and blocks just before it."); break;
            case 1: ImGui::TextDisabled("Blocks only while they are mid-swing."); break;
            case 2: ImGui::TextDisabled("Blocks whenever anyone is in reach. Blunt."); break;
            case 3: ImGui::TextDisabled("Never blocks on its own, only frees the key for your swings."); break;
        }

        // Live state, which is the fastest way to see it working
        ImGui::Separator();
        if (m_lastPredictMs >= 0) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.f, 1.f),
                "Next swing in ~%lld ms   (cycle %lld ms, %.1f CPS)",
                m_lastPredictMs, m_shownCycle,
                m_shownCycle > 0 ? 1000.0 / (double)m_shownCycle : 0.0);
        } else {
            ImGui::TextDisabled("No attacker tracked");
        }
        float cov = m_ticksTotal > 0 ? (100.f * m_ticksBlocked / m_ticksTotal) : 0.f;
        ImGui::TextDisabled("Blocking %.0f%% of the time | %s",
            cov, m_blocking ? "BLOCK" : "open");

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Range");
        ImGui::SliderFloat("Block Range", &m_blockRange, 2.f, 6.f, "%.1f");
        ImGui::SliderFloat("Detect Range", &m_detectRange, 3.f, 10.f, "%.1f");

        if (m_mode == 0) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Prediction");
            ImGui::SliderInt("Lead (ms)", &m_leadMs, 40, 300);
            ImGui::SliderInt("Assumed Cycle (ms)", &m_defaultCycle, 100, 400);
            ImGui::SliderInt("Stale After (ms)", &m_staleMs, 500, 3000);
            ImGui::SliderInt("Reaction Min (ms)", &m_reactionMin, 0, 150);
            ImGui::SliderInt("Reaction Max (ms)", &m_reactionMax, 0, 250);
            if (m_reactionMin > m_reactionMax) m_reactionMin = m_reactionMax;
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 1.f, 0.7f, 1.f), "Release");
        ImGui::SliderInt("Swing Gap (ticks)", &m_swingGap, 1, 4);
        ImGui::SliderInt("After Hit (ticks)", &m_afterHit, 0, 6);
        ImGui::SliderInt("Max Hold (ticks)", &m_maxBlockTicks, 10, 100);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "Humanisation");
        ImGui::SliderFloat("Chance", &m_chance, 50.f, 100.f, "%.0f%%");
        ImGui::Checkbox("Vary Timing", &m_varyTiming);
        if (m_varyTiming)
            ImGui::SliderFloat("Timing Noise", &m_timingNoise, 0.f, 60.f, "%.0f%%");

        ImGui::Separator();
        ImGui::Checkbox("Only Sword", &m_onlySword);
        ImGui::Checkbox("Pause On Flag", &m_pauseOnFlag);
        if (m_pauseOnFlag)
            ImGui::SliderInt("Flag Pause Ticks", &m_flagPauseTicks, 5, 60);

        if (!m_fSwinging && !m_fSwingProg) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "Swing field unresolved: prediction is blind, use In Range");
        }
        if (!KeyBinds::HasUseItem()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Use-item keybind unresolved: module inactive");
        }
    }
};
