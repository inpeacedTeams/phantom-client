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
#include <string>
#include <functional>
#include <chrono>
#include <random>
#include <cstring>
#include <cctype>
#include <cmath>

// =================================================================
// Auto Blockhit
// =================================================================
// Blocking with a sword in 1.8 halves incoming damage and resets
// sprint. Holding it permanently is useless and obvious, so this
// blocks only around the moment an enemy actually swings.
//
// THREE THINGS HAVE TO BE TRUE BEFORE THE BLOCK GOES DOWN
//   1. We are holding something that can block.
//   2. Someone is close enough to reach us AND facing us.
//   3. That someone has been swinging recently.
//
// Any one of them failing means the key stays up. An enemy standing
// next to you looking the other way is not a threat, and neither is
// one who has not thrown a punch in two seconds.
//
// THE BLOCK WINDOW
// EntityLivingBase raises isSwingInProgress the moment a player
// swings, which the client learns from the animation packet. Timing
// those rising edges gives their rhythm, usually a steady 100-250ms.
//
// From that we open a window:
//
//     predicted impact
//            |
//   ...------[==========]------...
//         lead        tail
//     block down    block up
//
// Outside the window the key is released, which is what lets you
// swing back. Once their hit lands, or the window closes without
// one arriving because they missed, it opens again.
//
// WHY THE ITEM CHECK IS FAIL-CLOSED
// Right-clicking with a sword blocks. Right-clicking with a block
// PLACES it, and with a bow it starts drawing. Blocking with the
// wrong item is worse than not blocking, so an unresolved item
// check disables the module rather than guessing.
// =================================================================

class AutoBlockhit : public Module {
private:
    using Clock = std::chrono::steady_clock;
    using Ms    = std::chrono::milliseconds;

    static long long MsSince(Clock::time_point t) {
        return std::chrono::duration_cast<Ms>(Clock::now() - t).count();
    }

    struct Enemy {
        // Swing tracking
        bool  wasSwinging = false;
        bool  hasSwung    = false;
        Clock::time_point lastSwing;
        std::deque<long long> intervals;
        long long avgInterval = 0;

        // Threat state, refreshed every tick
        double dist    = 99.0;
        bool   facing  = false;
        bool   inReach = false;
        unsigned long long lastSeen = 0;
    };

    // ---- Mode ----
    // 0 Predict, 1 Reactive, 2 In Range
    int m_mode = 0;

    // ---- Threat gate ----
    float m_blockRange   = 3.6f;   // they can reach us from here
    float m_detectRange  = 7.0f;   // start watching their rhythm
    bool  m_requireFacing = true;
    float m_facingFov    = 90.0f;  // cone their look must fall inside
    bool  m_requireAggro = true;
    int   m_aggroMs      = 1600;   // silence longer than this means calm

    // ---- Window ----
    int m_leadMs   = 140;   // open this long before the predicted impact
    int m_tailMs   = 120;   // stay down this long after a swing starts
    int m_defaultCycle = 220;
    int m_reactionMin  = 30;
    int m_reactionMax  = 90;

    // ---- Release ----
    int m_swingGap      = 1;    // ticks up so our own swing lands
    int m_afterHit      = 2;    // ticks up once their hit arrived
    int m_maxBlockTicks = 30;

    // ---- Humanisation ----
    float m_chance      = 95.0f;
    bool  m_varyTiming  = true;
    float m_timingNoise = 25.0f;

    // ---- Safety ----
    bool m_onlySword      = true;
    bool m_pauseOnFlag    = true;
    int  m_flagPauseTicks = 20;

    // ---- State ----
    std::unordered_map<int, Enemy> m_enemies;
    bool m_blocking       = false;
    int  m_releaseLeft    = 0;
    int  m_blockHeldTicks = 0;
    int  m_flagPause      = 0;
    bool m_lastLMB        = false;
    int  m_lastHurtTime   = 0;
    unsigned long long m_tick = 0;

    bool m_itemOk        = false;
    int  m_itemTimer     = 0;
    std::string m_heldName;

    // Live readout
    long long m_predictMs  = -1;
    long long m_shownCycle = 0;
    int  m_threats      = 0;
    int  m_tracked      = 0;
    int  m_ticksBlocked = 0;
    int  m_ticksTotal   = 0;
    const char* m_why   = "idle";

    // ---- JNI ----
    jfieldID  m_fInventory  = nullptr;
    jmethodID m_mCurItem    = nullptr;
    jmethodID m_mStackName  = nullptr;
    jfieldID  m_fSwinging   = nullptr;
    jfieldID  m_fSwingProg  = nullptr;
    jfieldID  m_fYaw        = nullptr;
    jmethodID m_getEntityId = nullptr;
    bool m_resolved   = false;
    bool m_itemUsable = false;   // can we tell what is in hand at all

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

    // -------------------------------------------------------------
    // Resolution
    //
    // The inventory field is looked up on the LIVE player object's
    // class rather than on ClassResolver::entityPlayer. Lunar's
    // hierarchy does not always match the vanilla one, and when that
    // lookup failed the old build silently allowed blocking with
    // anything, which is exactly the reported behaviour.
    // -------------------------------------------------------------
    void Resolve(JNIEnv* env) {
        if (m_resolved) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;      // retry next tick, still in a menu

        jclass pc = env->GetObjectClass(player);
        m_fInventory = JvmtiUtil::FindField(env, pc,
            { "field_71071_by", "inventory" });
        m_fYaw = JvmtiUtil::FindField(env, pc,
            { "field_70177_z", "rotationYaw" });

        m_fSwinging  = JvmtiUtil::FindField(env, pc,
            { "field_82175_bq", "isSwingInProgress" });
        m_fSwingProg = JvmtiUtil::FindField(env, pc,
            { "field_70733_aJ", "swingProgress" });

        m_getEntityId = JvmtiUtil::FindMethod(env, pc,
            { "func_145782_y", "getEntityId" }, 0);
        env->DeleteLocalRef(pc);

        if (m_fInventory) {
            jobject inv = env->GetObjectField(player, m_fInventory);
            if (inv) {
                jclass ic = env->GetObjectClass(inv);
                m_mCurItem = JvmtiUtil::FindMethod(env, ic,
                    { "func_70448_g", "getCurrentItem" }, 0);
                env->DeleteLocalRef(ic);
            }
        }

        m_itemUsable = (m_fInventory != nullptr && m_mCurItem != nullptr);
        m_resolved = true;

        printf("[AutoBlock] inv=%p item=%p swing=%p yaw=%p id=%p usable=%d\n",
            (void*)m_fInventory, (void*)m_mCurItem, (void*)m_fSwinging,
            (void*)m_fYaw, (void*)m_getEntityId, (int)m_itemUsable);
    }

    // -------------------------------------------------------------
    // What is in hand
    //
    // Unlocalised names are literal strings in the source and come
    // through obfuscation untouched, so "item.swordDiamond" is far
    // more dependable than chasing the ItemSword class by field.
    // -------------------------------------------------------------
    bool CheckHeldItem(JNIEnv* env, jobject player) {
        m_heldName.clear();

        if (!m_onlySword) return true;
        if (!m_itemUsable) return false;         // cannot tell, so do not

        jobject inv = env->GetObjectField(player, m_fInventory);
        if (!inv) return false;

        jobject stack = env->CallObjectMethod(inv, m_mCurItem);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!stack) { m_heldName = "empty"; return false; }

        if (!m_mStackName) {
            jclass sc = env->GetObjectClass(stack);
            m_mStackName = JvmtiUtil::FindMethod(env, sc,
                { "func_77977_a", "getUnlocalizedName" }, 0);
            env->DeleteLocalRef(sc);
        }
        if (!m_mStackName) return false;

        jstring js = (jstring)env->CallObjectMethod(stack, m_mStackName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!js) return false;

        bool ok = false;
        const char* raw = env->GetStringUTFChars(js, nullptr);
        if (raw) {
            char low[160];
            size_t n = std::strlen(raw);
            if (n > sizeof(low) - 1) n = sizeof(low) - 1;
            for (size_t i = 0; i < n; i++)
                low[i] = (char)std::tolower((unsigned char)raw[i]);
            low[n] = '\0';

            m_heldName = low;
            ok = (std::strstr(low, "sword") != nullptr);

            env->ReleaseStringUTFChars(js, raw);
        }
        env->DeleteLocalRef(js);
        return ok;
    }

    bool IsSwinging(JNIEnv* env, jobject ent) {
        if (m_fSwinging)  return env->GetBooleanField(ent, m_fSwinging) != 0;
        if (m_fSwingProg) return env->GetFloatField(ent, m_fSwingProg) > 0.001f;
        return false;
    }

    // Is this entity looking at us? Someone facing away cannot land
    // a hit, so they should never pull the block down.
    bool FacingUs(JNIEnv* env, jobject ent,
                  double ex, double ez, double px, double pz)
    {
        if (!m_requireFacing) return true;
        if (!m_fYaw) return true;              // cannot tell, assume yes

        float yaw = env->GetFloatField(ent, m_fYaw);
        const double DEG = 3.14159265358979 / 180.0;

        double lx = -std::sin(yaw * DEG);
        double lz =  std::cos(yaw * DEG);

        double dx = px - ex, dz = pz - ez;
        double len = std::sqrt(dx*dx + dz*dz);
        if (len < 0.001) return true;          // on top of us
        dx /= len; dz /= len;

        double dot = lx * dx + lz * dz;
        if (dot > 1.0) dot = 1.0;
        if (dot < -1.0) dot = -1.0;

        double angle = std::acos(dot) / DEG;
        return angle <= (m_facingFov * 0.5);
    }

    void SetBlock(JNIEnv* env, bool on, const char* why) {
        m_why = why;
        if (m_blocking == on) return;
        KeyBinds::SetUseItem(env, on);
        m_blocking = on;
        if (!on) m_blockHeldTicks = 0;
    }

public:
    AutoBlockhit()
        : Module("Auto Blockhit", "Blocks around the enemy's swing, not always",
                 ModuleCategory::COMBAT, 0)
    {
        Bind("Mode", &m_mode);
        Bind("Block Range", &m_blockRange);
        Bind("Detect Range", &m_detectRange);
        Bind("Require Facing", &m_requireFacing);
        Bind("Facing FOV", &m_facingFov);
        Bind("Require Aggro", &m_requireAggro);
        Bind("Aggro Window", &m_aggroMs);
        Bind("Lead", &m_leadMs);
        Bind("Tail", &m_tailMs);
        Bind("Default Cycle", &m_defaultCycle);
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

        if (Minecraft::IsInGui(env)) { SetBlock(env, false, "menu"); return; }

        if (m_flagPause > 0) {
            m_flagPause--;
            SetBlock(env, false, "flagged");
            return;
        }

        // ---- What are we holding ----
        // Walking the inventory every tick is wasteful and the held
        // item rarely changes mid-swing.
        if (--m_itemTimer <= 0) {
            m_itemOk = CheckHeldItem(env, player);
            m_itemTimer = 5;
        }
        if (!m_itemOk) { SetBlock(env, false, "no sword"); return; }

        m_ticksTotal++;
        if (m_blocking) m_ticksBlocked++;
        if (m_ticksTotal > 400) { m_ticksTotal /= 2; m_ticksBlocked /= 2; }

        // ---- Our own swing frees the key ----
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool weSwung = lmb && !m_lastLMB;
        m_lastLMB = lmb;
        if (weSwung) {
            SetBlock(env, false, "our swing");
            m_releaseLeft = m_swingGap;
        }

        // ---- Their hit landed, the block did its job ----
        int hurt = Minecraft::GetHurtTime(env, player);
        if (hurt > 0 && m_lastHurtTime == 0) {
            SetBlock(env, false, "hit passed");
            m_releaseLeft = m_afterHit;
        }
        m_lastHurtTime = hurt;

        // ---- Survey everyone nearby ----
        if (!EntityList::Init(env)) return;
        auto ents = EntityList::GetPlayers(env, m_detectRange);

        m_tracked = (int)ents.size();
        m_threats = 0;
        m_predictMs = -1;
        m_shownCycle = 0;

        if (ents.empty()) {
            SetBlock(env, false, "nobody near");
            return;
        }

        double px = Minecraft::GetPosX(env, player);
        double pz = Minecraft::GetPosZ(env, player);

        bool swingingNow = false;   // a threat is mid-swing right now
        bool anyThreat   = false;   // a threat exists at all
        long long soonest = -1;     // ms until the next predicted impact
        long long sinceLast = -1;   // ms since a threat last swung

        for (auto& e : ents) {
            int id;
            if (m_getEntityId) {
                jint v = env->CallIntMethod(e.ref, m_getEntityId);
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                id = (int)v;
            } else {
                // No entity id available: names are stable enough to
                // key rhythm tracking on.
                id = (int)std::hash<std::string>{}(e.name);
            }

            Enemy& en = m_enemies[id];
            en.lastSeen = m_tick;
            en.dist = e.distanceToPlayer;
            en.inReach = (e.distanceToPlayer <= m_blockRange);
            en.facing = FacingUs(env, e.ref, e.posX, e.posZ, px, pz);

            // ---- Rhythm ----
            // The rising edge is the swing. Holding the button keeps
            // the flag up for several ticks afterwards, so only the
            // transition counts.
            bool swinging = IsSwinging(env, e.ref);
            if (swinging && !en.wasSwinging) {
                if (en.hasSwung) {
                    long long gap = MsSince(en.lastSwing);
                    // Discard nonsense: duplicate events, or a pause
                    // so long it says nothing about their rhythm.
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

            // ---- Is this person actually dangerous ----
            if (!en.inReach) continue;
            if (m_requireFacing && !en.facing) continue;

            long long since = en.hasSwung ? MsSince(en.lastSwing) : -1;

            // Someone who has not thrown a punch recently is not
            // attacking us, whatever else they are doing.
            if (m_requireAggro) {
                if (!en.hasSwung) continue;
                if (since > m_aggroMs) continue;
            }

            anyThreat = true;
            m_threats++;

            if (swinging) swingingNow = true;
            if (since >= 0 && (sinceLast < 0 || since < sinceLast))
                sinceLast = since;

            if (!en.hasSwung) continue;

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

        m_predictMs = soonest;

        // ---- Still inside a forced release ----
        if (m_releaseLeft > 0) {
            m_releaseLeft--;
            SetBlock(env, false, "recovering");
            return;
        }

        // ---- No threat means no block, whatever the mode ----
        if (!anyThreat) {
            SetBlock(env, false,
                     m_tracked ? "no threat" : "nobody near");
            return;
        }

        // ---- Decide ----
        bool want = false;
        const char* why = "open";

        switch (m_mode) {
            case 1:   // Reactive: only while an arm is actually moving
                want = swingingNow;
                why = want ? "mid-swing" : "waiting";
                break;

            case 2:   // In Range: any qualified threat is enough
                want = true;
                why = "in range";
                break;

            default: { // Predict
                if (swingingNow) {
                    want = true;
                    why = "mid-swing";
                } else if (sinceLast >= 0 && sinceLast <= m_tailMs) {
                    // Their swing just went out and the hit may still
                    // be in flight, so stay down a moment longer.
                    want = true;
                    why = "impact window";
                } else if (soonest >= 0) {
                    long long lead = Noisy(m_leadMs)
                                   + Rand(m_reactionMin, m_reactionMax);
                    want = (soonest <= lead);
                    why = want ? "pre-swing" : "waiting";
                }
                break;
            }
        }

        // Holding forever is both a tell and pointless
        if (want && m_blocking && m_blockHeldTicks >= m_maxBlockTicks) {
            SetBlock(env, false, "held too long");
            m_releaseLeft = 2;
            return;
        }

        if (want && !m_blocking) {
            if (!Roll(m_chance)) { m_why = "missed one"; return; }
            SetBlock(env, true, why);
        } else if (!want && m_blocking) {
            SetBlock(env, false, why);
        } else {
            m_why = why;
        }

        if (m_blocking) m_blockHeldTicks++;
    }

    void OnDisable(JNIEnv* env) override {
        SetBlock(env, false, "off");
        m_releaseLeft = 0;
        m_blockHeldTicks = 0;
        m_enemies.clear();
        m_predictMs = -1;
    }

    void OnServerCorrection() {
        if (m_pauseOnFlag) m_flagPause = m_flagPauseTicks;
    }

    void RenderSettings() override {
        const char* modes[] = { "Predict", "Reactive", "In Range" };
        ImGui::Combo("Mode", &m_mode, modes, 3);
        switch (m_mode) {
            case 0: ImGui::TextDisabled("Learns their rhythm and blocks just before the swing."); break;
            case 1: ImGui::TextDisabled("Blocks only while an arm is actually moving."); break;
            case 2: ImGui::TextDisabled("Blocks whenever a facing enemy is in reach."); break;
        }

        // ---- Live state, the fastest way to see what it is doing ----
        ImGui::Separator();
        ImGui::TextColored(m_blocking ? ImVec4(0.2f, 0.8f, 0.4f, 1.f)
                                      : ImVec4(0.55f, 0.55f, 0.6f, 1.f),
            "%s  (%s)", m_blocking ? "BLOCKING" : "open", m_why);

        ImGui::TextDisabled("Nearby %d | threats %d | holding: %s",
            m_tracked, m_threats,
            m_heldName.empty() ? "?" : m_heldName.c_str());

        if (m_predictMs >= 0) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.f, 1.f),
                "Next swing ~%lld ms  (cycle %lld ms, %.1f CPS)",
                m_predictMs, m_shownCycle,
                m_shownCycle > 0 ? 1000.0 / (double)m_shownCycle : 0.0);
        }

        float cov = m_ticksTotal > 0 ? (100.f * m_ticksBlocked / m_ticksTotal) : 0.f;
        ImGui::TextDisabled("Down %.0f%% of the time", cov);

        // ---- Threat gate ----
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Threat");
        ImGui::SliderFloat("Block Range", &m_blockRange, 2.f, 6.f, "%.1f");
        ImGui::SliderFloat("Detect Range", &m_detectRange, 3.f, 12.f, "%.1f");
        ImGui::Checkbox("Require Facing", &m_requireFacing);
        if (m_requireFacing)
            ImGui::SliderFloat("Facing FOV", &m_facingFov, 30.f, 200.f, "%.0f");
        ImGui::Checkbox("Require Aggression", &m_requireAggro);
        if (m_requireAggro)
            ImGui::SliderInt("Aggro Window (ms)", &m_aggroMs, 400, 4000);
        ImGui::TextDisabled("All of these must pass or the key stays up.");

        // ---- Window ----
        if (m_mode == 0) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "Window");
            ImGui::SliderInt("Lead (ms)", &m_leadMs, 40, 320);
            ImGui::SliderInt("Tail (ms)", &m_tailMs, 0, 300);
            ImGui::SliderInt("Assumed Cycle (ms)", &m_defaultCycle, 100, 400);
            ImGui::SliderInt("Reaction Min (ms)", &m_reactionMin, 0, 150);
            ImGui::SliderInt("Reaction Max (ms)", &m_reactionMax, 0, 250);
            if (m_reactionMin > m_reactionMax) m_reactionMin = m_reactionMax;
        }

        // ---- Release ----
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 1.f, 0.7f, 1.f), "Release");
        ImGui::SliderInt("Swing Gap (ticks)", &m_swingGap, 1, 4);
        ImGui::SliderInt("After Hit (ticks)", &m_afterHit, 0, 6);
        ImGui::SliderInt("Max Hold (ticks)", &m_maxBlockTicks, 8, 80);

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

        // ---- Diagnostics ----
        if (m_onlySword && !m_itemUsable) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f),
                "Held item unreadable: module disabled");
            ImGui::TextDisabled("Blocking blind would place blocks or draw a bow. "
                                "Turn off Only Sword to override.");
        }
        if (!m_fSwinging && !m_fSwingProg) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f),
                "Swing field unresolved: prediction is blind, use In Range");
        }
        if (m_requireFacing && !m_fYaw) {
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                "Enemy yaw unresolved: facing check always passes");
        }
        if (!KeyBinds::HasUseItem()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                "Use-item keybind unresolved: module inactive");
        }
    }
};
