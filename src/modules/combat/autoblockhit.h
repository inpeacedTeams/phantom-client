#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/entity_list.h"
#include "../../mc/combat_state.h"
#include "../../mc/movement.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include "../../util/log.h"
#include <unordered_map>
#include <deque>
#include <string>
#include <functional>
#include <chrono>
#include <random>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cmath>

// =================================================================
// Auto Blockhit
// =================================================================
// Blocking with a sword in 1.8 halves incoming damage and resets
// sprint. Holding it permanently is useless and obvious, so this
// blocks only around the moment an enemy actually swings.
//
// -----------------------------------------------------------------
// BLOCKING COSTS 80% OF YOUR SPEED
// -----------------------------------------------------------------
// EntityPlayer.onLivingUpdate:
//
//     if (isBlocking()) {
//         moveStrafing *= 0.2F;
//         moveForward  *= 0.2F;
//     }
//
// That is not a small tax. A blocking player moves at a fifth of
// walking speed, which in a fight feels exactly like being frozen.
//
// So there is a movement budget: a hard cap on how long the block
// may stay down while you are trying to walk, and a forced gap
// afterwards. Damage reduction is worth having, but not at the
// price of being unable to chase or retreat.
//
// -----------------------------------------------------------------
// AND IT USED TO STEAL YOUR RIGHT MOUSE BUTTON
// -----------------------------------------------------------------
// Releasing the block called Set(useItem, false), which is not a
// release: it takes an override and pins the key DOWN in the false
// position. Nothing ever handed it back, so after the first block
// the player's own right click was dead. Now it calls Release,
// which restores whatever the mouse is really doing.
//
// -----------------------------------------------------------------
// THREE THINGS HAVE TO BE TRUE BEFORE THE BLOCK GOES DOWN
//   1. We are holding something that can block.
//   2. Someone is close enough to reach us AND facing us.
//   3. That someone has been swinging recently.
//
// THE BLOCK WINDOW
//     predicted impact
//            |
//   ...------[==========]------...
//         lead        tail
//     block down    block up
//
// WHY THE ITEM CHECK IS FAIL-CLOSED
// Right-clicking with a sword blocks. With a block it PLACES one,
// with a bow it starts drawing. Blocking with the wrong item is
// worse than not blocking, so an unresolved item check disables the
// module rather than guessing.
//
// WHY THERE IS AN OnReset
// m_blocking is our belief about a key we do not own. A respawn or
// a server change releases every key from underneath us, and a
// module still believing it holds the block will never press it
// again, because SetBlock only acts on a change. It looked exactly
// like the module had died.
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

        double dist    = 99.0;
        bool   facing  = false;
        bool   inReach = false;
        unsigned long long lastSeen = 0;
    };

    enum Mode { PREDICT = 0, REACTIVE, IN_RANGE };

    static constexpr const char* kModes[] = { "Predict", "Reactive", "In Range" };

    // ---- Core ----
    int   m_mode       = PREDICT;
    float m_blockRange = 3.6f;
    float m_chance     = 95.0f;

    // ---- Advanced: threat gate ----
    float m_detectRange   = 7.0f;
    bool  m_requireFacing = true;
    float m_facingFov     = 90.0f;
    bool  m_requireAggro  = true;
    int   m_aggroMs       = 1600;

    // ---- Advanced: window ----
    int m_leadMs       = 140;
    int m_tailMs       = 120;
    int m_defaultCycle = 220;
    int m_reactionMin  = 30;
    int m_reactionMax  = 90;

    // ---- Advanced: release ----
    int m_swingGap      = 1;
    int m_afterHit      = 2;
    int m_maxBlockTicks = 12;

    // ---- Advanced: movement budget ----
    bool m_protectMovement  = true;
    int  m_maxMovingTicks   = 6;   // consecutive ticks blocked while walking
    int  m_movingCooldown   = 4;   // forced open ticks afterwards
    bool m_neverWhileSprint = false;

    // ---- Advanced: humanisation and safety ----
    bool  m_varyTiming  = true;
    float m_timingNoise = 25.0f;
    bool  m_onlySword      = true;
    bool  m_pauseOnFlag    = true;
    int   m_flagPauseTicks = 20;

    // ---- State ----
    std::unordered_map<int, Enemy> m_enemies;
    bool m_blocking       = false;
    int  m_releaseLeft    = 0;
    int  m_blockHeldTicks = 0;
    int  m_movingHeld     = 0;
    int  m_flagPause      = 0;
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
    int  m_speedSaves   = 0;
    const char* m_why   = "idle";
    char m_status[56]   = { 0 };
    mutable char m_notice[200] = { 0 };

    // ---- Tuning ----
    static constexpr int kItemRecheckTicks = 5;   // 4x a second is plenty
    static constexpr int kForgetTicks      = 60;
    static constexpr int kCoverageWindow   = 400; // ticks before the ratio decays
    static constexpr long long kMinSwingGapMs = 60;
    static constexpr long long kMaxSwingGapMs = 1200;
    static constexpr size_t kIntervalWindow = 8;

    // Past this share of the fight the module is costing you more
    // in movement than it saves in damage.
    static constexpr float kCoverageWarn = 55.0f;

    // ---- JNI ----
    jfieldID  m_fInventory  = nullptr;
    jmethodID m_mCurItem    = nullptr;
    jmethodID m_mStackName  = nullptr;
    jfieldID  m_fSwinging   = nullptr;
    jfieldID  m_fSwingProg  = nullptr;
    jfieldID  m_fYaw        = nullptr;
    jmethodID m_getEntityId = nullptr;
    bool m_resolved   = false;
    bool m_itemUsable = false;

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

    float Coverage() const {
        return m_ticksTotal > 0 ? (100.0f * m_ticksBlocked / m_ticksTotal) : 0.0f;
    }

    // -------------------------------------------------------------
    // Resolution
    //
    // The inventory field is looked up on the LIVE player object's
    // class rather than on ClassResolver::entityPlayer. Lunar's
    // hierarchy does not always match the vanilla one.
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

        PH_LOG("[AutoBlock] inv=%p item=%p swing=%p yaw=%p id=%p usable=%d",
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

        double lx = -std::sin(yaw * Movement::kDegToRad);
        double lz =  std::cos(yaw * Movement::kDegToRad);

        double dx = px - ex, dz = pz - ez;
        double len = std::sqrt(dx*dx + dz*dz);
        if (len < 0.001) return true;          // on top of us
        dx /= len; dz /= len;

        double dot = lx * dx + lz * dz;
        if (dot > 1.0) dot = 1.0;
        if (dot < -1.0) dot = -1.0;

        double angle = std::acos(dot) / Movement::kDegToRad;
        return angle <= (m_facingFov * 0.5);
    }

    // Down uses an override. UP must be a RELEASE, not an override
    // to false, or the player's own right mouse button stays dead
    // for the rest of the session.
    void SetBlock(JNIEnv* env, bool on, const char* why) {
        m_why = why;
        if (m_blocking == on) return;

        if (on) KeyBinds::SetUseItem(env, true);
        else    KeyBinds::ReleaseUseItem(env);

        m_blocking = on;
        if (!on) m_blockHeldTicks = 0;
    }

    // Stand down and clear the collapsed-row readout, so a stale
    // "blocking" does not sit in the menu after we have stopped.
    void StandDown(JNIEnv* env, const char* why) {
        SetBlock(env, false, why);
        m_status[0] = '\0';
    }

public:
    AutoBlockhit()
        : Module("Auto Blockhit", "Blocks around the enemy's swing, not always",
                 ModuleCategory::COMBAT, 0)
    {
        BindMode("Mode", &m_mode, kModes, 3,
                 "Predict learns their rhythm and blocks just before the "
                 "swing. Reactive waits until an arm is actually moving. "
                 "In Range blocks whenever a facing enemy is close.");

        Bind("Reach", &m_blockRange, 2.0f, 6.0f, "%.1f",
             "How close someone has to be to be worth blocking");

        Bind("Chance", &m_chance, 50.0f, 100.0f, "%.0f%%",
             "How many opportunities are taken");

        // ---- Movement ----
        Bind("Protect Movement", &m_protectMovement,
             "Blocking cuts your speed to a fifth. This caps how long it "
             "can hold you while you are trying to walk.")
            .Advanced();

        Bind("Max Ticks Walking", &m_maxMovingTicks, 2, 20,
             "Longest the block may hold while you are moving")
            .When("Protect Movement", 1).Advanced();

        Bind("Cooldown After", &m_movingCooldown, 1, 12,
             "Forced open ticks once the budget runs out")
            .When("Protect Movement", 1).Advanced();

        Bind("Never While Sprinting", &m_neverWhileSprint,
             "Keeps your sprint under any circumstances")
            .Advanced();

        // ---- Threat gate ----
        Bind("Detect Range", &m_detectRange, 3.0f, 12.0f, "%.1f",
             "How far out enemies are watched and their rhythm learned")
            .Advanced();

        Bind("Require Facing", &m_requireFacing,
             "Someone looking away cannot land a hit")
            .Advanced();

        Bind("Facing FOV", &m_facingFov, 30.0f, 200.0f, "%.0f",
             "How far off centre still counts as looking at you")
            .When("Require Facing", 1).Advanced();

        Bind("Require Aggression", &m_requireAggro,
             "Only treat someone as a threat once they have swung")
            .Advanced();

        Bind("Aggro Window", &m_aggroMs, 400, 4000,
             "Milliseconds a swing keeps counting for")
            .When("Require Aggression", 1).Advanced();

        // ---- Window ----
        Bind("Lead", &m_leadMs, 40, 320,
             "Milliseconds before the predicted swing that the block drops")
            .When("Mode", PREDICT).Advanced();

        Bind("Tail", &m_tailMs, 0, 300,
             "How long it stays down afterwards")
            .When("Mode", PREDICT).Advanced();

        Bind("Assumed Cycle", &m_defaultCycle, 100, 400,
             "Swing interval assumed before one has been measured")
            .When("Mode", PREDICT).Advanced();

        Bind("Reaction Min", &m_reactionMin, 0, 150,
             "A human does not react instantly, and neither should this")
            .When("Mode", PREDICT).Advanced();

        Bind("Reaction Max", &m_reactionMax, 0, 250)
            .When("Mode", PREDICT).Advanced();

        // ---- Release ----
        Bind("Swing Gap", &m_swingGap, 1, 4,
             "Ticks the block stays open after your own swing")
            .Advanced();

        Bind("After Hit", &m_afterHit, 0, 6,
             "Ticks it stays open once their hit has landed")
            .Advanced();

        Bind("Max Hold", &m_maxBlockTicks, 4, 40,
             "Nothing legitimate holds a block longer than this")
            .Advanced();

        // ---- Safety ----
        Bind("Vary Timing", &m_varyTiming,
             "Jitters the window so the same interval never repeats")
            .Advanced();

        Bind("Timing Noise", &m_timingNoise, 0.0f, 60.0f, "%.0f%%",
             "How much the window wanders")
            .When("Vary Timing", 1).Advanced();

        Bind("Only Sword", &m_onlySword,
             "Right-clicking anything else places a block or draws a bow")
            .Advanced();

        Bind("Pause On Flag", &m_pauseOnFlag,
             "Stand down for a moment after the server corrects you")
            .Advanced();

        Bind("Flag Pause Ticks", &m_flagPauseTicks, 5, 60)
            .When("Pause On Flag", 1).Advanced();
    }

    void OnTick(JNIEnv* env) override {
        if (!KeyBinds::Init(env)) return;
        Resolve(env);

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { StandDown(env, "no player"); return; }

        m_tick++;

        if (m_reactionMin > m_reactionMax) m_reactionMin = m_reactionMax;

        if (Minecraft::IsInGui(env)) { StandDown(env, "menu"); return; }

        if (m_flagPause > 0) {
            m_flagPause--;
            StandDown(env, "flagged");
            return;
        }

        // ---- What are we holding ----
        if (--m_itemTimer <= 0) {
            m_itemOk = CheckHeldItem(env, player);
            m_itemTimer = kItemRecheckTicks;
        }
        if (!m_itemOk) { StandDown(env, "no sword"); return; }

        m_ticksTotal++;
        if (m_blocking) m_ticksBlocked++;
        if (m_ticksTotal > kCoverageWindow) { m_ticksTotal /= 2; m_ticksBlocked /= 2; }

        // ---- Our own swing frees the key ----
        // Driven by the real swing rather than the mouse button, so
        // it stays in step with the packet the server sees, and it
        // still fires when the clicks come from the click queue.
        if (CombatState::SwungThisTick()) {
            SetBlock(env, false, "our swing");
            m_releaseLeft = m_swingGap;
        }

        // ---- Their hit landed, the block did its job ----
        // CombatState owns the hurtTime edge for the whole client.
        if (CombatState::HitTakenThisTick()) {
            SetBlock(env, false, "hit passed");
            m_releaseLeft = m_afterHit;
        }

        // =========================================================
        // Movement budget
        // =========================================================
        bool walking = KeyBinds::PhysMoving(env);

        if (m_protectMovement && walking && m_blocking) {
            m_movingHeld++;
            if (m_movingHeld > m_maxMovingTicks) {
                m_speedSaves++;
                m_movingHeld = 0;
                SetBlock(env, false, "movement budget");
                m_releaseLeft = m_movingCooldown;
                return;
            }
        } else if (!m_blocking) {
            m_movingHeld = 0;
        }

        if (m_neverWhileSprint && Minecraft::HasSprintCheck()
            && Minecraft::IsSprinting(env, player)) {
            StandDown(env, "sprinting");
            return;
        }

        // ---- Survey everyone nearby ----
        if (!EntityList::Init(env)) return;
        auto ents = EntityList::GetPlayers(env, m_detectRange);

        m_tracked = (int)ents.size();
        m_threats = 0;
        m_predictMs = -1;
        m_shownCycle = 0;

        if (ents.empty()) {
            StandDown(env, "nobody near");
            return;
        }

        double px = Minecraft::GetPosX(env, player);
        double pz = Minecraft::GetPosZ(env, player);

        bool swingingNow = false;
        bool anyThreat   = false;
        long long soonest = -1;
        long long sinceLast = -1;

        for (auto& e : ents) {
            int id;
            if (m_getEntityId) {
                jint v = env->CallIntMethod(e.ref, m_getEntityId);
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                id = (int)v;
            } else {
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
                    if (gap >= kMinSwingGapMs && gap <= kMaxSwingGapMs) {
                        en.intervals.push_back(gap);
                        if (en.intervals.size() > kIntervalWindow)
                            en.intervals.pop_front();
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
            if (m_tick - it->second.lastSeen > kForgetTicks) it = m_enemies.erase(it);
            else ++it;
        }

        m_predictMs = soonest;

        // ---- Still inside a forced release ----
        if (m_releaseLeft > 0) {
            m_releaseLeft--;
            StandDown(env, "recovering");
            return;
        }

        // ---- No threat means no block, whatever the mode ----
        if (!anyThreat) {
            StandDown(env, m_tracked ? "no threat" : "nobody near");
            return;
        }

        // ---- Decide ----
        bool want = false;
        const char* why = "open";

        switch (m_mode) {
            case REACTIVE:
                want = swingingNow;
                why = want ? "mid-swing" : "waiting";
                break;

            case IN_RANGE:
                want = true;
                why = "in range";
                break;

            default: {
                if (swingingNow) {
                    want = true;
                    why = "mid-swing";
                } else if (sinceLast >= 0 && sinceLast <= m_tailMs) {
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

        // Collapsed-row summary
        if (m_blocking)      snprintf(m_status, sizeof(m_status), "blocking");
        else if (m_threats)  snprintf(m_status, sizeof(m_status), "%d threat%s",
                                      m_threats, m_threats == 1 ? "" : "s");
        else                 m_status[0] = '\0';
    }

    void OnDisable(JNIEnv* env) override {
        SetBlock(env, false, "off");
        // Backstop: whatever we thought our state was, the right
        // mouse button goes back to the player.
        if (env) KeyBinds::ReleaseUseItem(env);
        m_releaseLeft = 0;
        m_blockHeldTicks = 0;
        m_movingHeld = 0;
        m_enemies.clear();
        m_predictMs = -1;
        m_status[0] = '\0';
    }

    // -------------------------------------------------------------
    // World change, respawn, reconnect.
    //
    // Two things are stale. Every swing rhythm belongs to players
    // who are no longer here, and m_blocking describes a key that
    // has already been released underneath us. The second one is
    // the dangerous half: SetBlock is edge-triggered, so a module
    // that still believes it is blocking will never press again.
    // -------------------------------------------------------------
    void OnReset(JNIEnv* env) override {
        if (env) KeyBinds::ReleaseUseItem(env);
        m_blocking       = false;
        m_why            = "idle";
        m_releaseLeft    = 0;
        m_blockHeldTicks = 0;
        m_movingHeld     = 0;
        m_flagPause      = 0;
        m_itemOk         = false;
        m_itemTimer      = 0;   // re-read the hand on the very next tick
        m_heldName.clear();
        m_enemies.clear();
        m_predictMs   = -1;
        m_shownCycle  = 0;
        m_threats     = 0;
        m_tracked     = 0;
        m_ticksBlocked = 0;
        m_ticksTotal   = 0;
        m_status[0]   = '\0';
    }

    void OnServerCorrection() {
        if (m_pauseOnFlag) m_flagPause = m_flagPauseTicks;
    }

    const char* StatusLine() const override {
        return m_status[0] ? m_status : nullptr;
    }

    NoticeLevel Notice(const char** text) const override {
        if (m_onlySword && !m_itemUsable) {
            *text = "The held item cannot be read in this build of the game. "
                    "Blocking blind would place blocks or draw a bow, so the "
                    "module stays off.";
            return NoticeLevel::Danger;
        }
        if (!KeyBinds::HasUseItem()) {
            *text = "The use-item keybind could not be found, so the block "
                    "cannot be pressed.";
            return NoticeLevel::Danger;
        }
        if (m_mode == PREDICT && !m_fSwinging && !m_fSwingProg) {
            *text = "Enemy swings cannot be seen in this build, so prediction "
                    "is blind. Use In Range instead.";
            return NoticeLevel::Warning;
        }

        float cov = Coverage();
        if (cov > kCoverageWarn) {
            snprintf(m_notice, sizeof(m_notice),
                     "Blocking %.0f%% of the time. That is most of the fight "
                     "spent at a fifth of your walking speed.", cov);
            *text = m_notice;
            return NoticeLevel::Warning;
        }
        if (m_ticksTotal > 40) {
            snprintf(m_notice, sizeof(m_notice),
                     "Blocking %.0f%% of the time, holding %s.", cov,
                     m_heldName.empty() ? "nothing readable" : m_heldName.c_str());
            *text = m_notice;
            return NoticeLevel::Info;
        }
        return NoticeLevel::None;
    }
};
