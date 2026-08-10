#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../input/focus.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <Windows.h>
#include <chrono>
#include <deque>
#include <random>
#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdio>

// =================================================================
// AutoClicker
// =================================================================
// Clicks the held mouse button for you, and does it the way a hand
// does rather than the way a loop does.
//
// -----------------------------------------------------------------
// WHY THERE IS NO SEPARATE TIMER THREAD
// -----------------------------------------------------------------
// Minecraft.runTick() consumes KeyBinding.pressTime on the tick
// boundary:
//
//     while (gameSettings.keyBindAttack.isPressed()) clickMouse();
//
// So a click reaches the server QUANTISED TO A TICK, 50ms, however
// precisely it was scheduled. Whatever sub-tick timing a 1ms thread
// produces is flattened before it leaves the client and the server
// never sees it. Everything the server CAN see is therefore fully
// described by one question asked once per tick: do we click this
// tick or not.
//
// That is why this runs on the client tick and nowhere else. No
// third thread, no atomics, no callback into module memory from
// another thread, none of the hazards the old design carried. The
// hard consequence is the honest one: at most ONE click per tick.
// Two clicks in a tick is a sub-20ms interval, which no hand makes
// and every anticheat that bothers to look will flag, so the
// ceiling is ~20 CPS and the module does not pretend otherwise.
//
// -----------------------------------------------------------------
// WHAT A HUMAN CLICK STREAM LOOKS LIKE, WITHIN THAT CONSTRAINT
// -----------------------------------------------------------------
//   * intervals are NORMALLY distributed around the target, not
//     uniform: a flat histogram is a machine signature;
//   * the target rate DRIFTS slowly and is correlated tick to tick,
//     because a hand speeds up and eases off rather than holding a
//     dead-flat rate;
//   * FATIGUE: the occasional longer gap where attention slips;
//   * BUTTERFLY: a pair of clicks one tick apart among the longer
//     gaps, the tick-quantised version of two fingers alternating,
//     for anticheats that expect a double-click structure at speed.
//
// A fractional tick countdown carries its remainder between clicks,
// so a target that is not a whole number of ticks (12 CPS is 1.67
// ticks) comes out at the right AVERAGE instead of snapping to 10
// or 20.
// =================================================================

class AutoClicker : public Module {
private:
    using Clock = std::chrono::steady_clock;

    static constexpr const char* kButtons[]  = { "Left (attack)", "Right (use)" };
    static constexpr const char* kPatterns[] = { "Jitter", "Butterfly" };

    // One click per tick is the hard cap; 20 TPS puts the ceiling
    // here. Nothing above it can reach the server as a real click.
    static constexpr float kMaxCps = 20.0f;

    // Above this a sustained rate reads as butterfly rather than a
    // plain click, and people notice on a recording.
    static constexpr int kNoisyCps = 16;

    // ---- Core ----
    int m_button = 0;      // 0 attack, 1 use item
    int m_cpsMin = 8;
    int m_cpsMax = 12;

    // ---- Pattern / humanisation ----
    int   m_pattern      = 0;      // 0 jitter, 1 butterfly
    bool  m_randomize    = true;
    float m_variance     = 22.0f;  // percent spread on the interval
    bool  m_fatigue      = true;
    float m_fatigueChance = 4.0f;
    float m_burstChance  = 12.0f;  // butterfly: how often a pair fires

    // ---- Gating ----
    bool  m_breakSafe     = true;   // do not fight block breaking
    bool  m_weaponsOnly   = false;  // only with a sword/axe in hand
    bool  m_requireTarget = false;
    float m_targetRange   = 4.0f;

    // ---- Scheduler state (client thread only) ----
    float m_nextInTicks = 0.0f;    // fractional tick countdown
    int   m_burstLeft    = 0;      // forced 1-tick gaps still owed
    double m_drift       = 0.0;    // slow correlated rate wander, [-1,1]
    bool  m_wasHolding   = false;

    // ---- Readout ----
    std::deque<Clock::time_point> m_fires;   // recent clicks, for live CPS
    const char* m_why = "idle";
    mutable char m_status[48] = {};
    mutable char m_notice[176] = {};

    // ---- JNI ----
    jfieldID  m_fHittingBlock = nullptr;   // PlayerControllerMP.isHittingBlock
    jfieldID  m_fInventory    = nullptr;   // EntityPlayer.inventory
    jmethodID m_mCurItem      = nullptr;   // InventoryPlayer.getCurrentItem
    jmethodID m_mStackName    = nullptr;   // ItemStack.getUnlocalizedName
    bool m_resolved = false;

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        return std::uniform_real_distribution<float>(0.f, 100.f)(m_rng) < pct;
    }

    // Box-Muller, so intervals are Gaussian rather than uniform.
    double Gauss(double mean, double sigma) {
        double u1 = std::uniform_real_distribution<double>(1e-9, 1.0)(m_rng);
        double u2 = std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
        double z0 = std::sqrt(-2.0 * std::log(u1))
                  * std::cos(6.283185307179586 * u2);
        return mean + sigma * z0;
    }

    int VkForButton() const { return m_button == 1 ? VK_RBUTTON : VK_LBUTTON; }

    float CeilingClampedMax() const {
        float hi = (float)m_cpsMax;
        return hi > kMaxCps ? kMaxCps : hi;
    }

    // -------------------------------------------------------------
    // The target rate for THIS click. Drifts slowly between the min
    // and max the player set, so neighbouring clicks are related the
    // way a hand's rate is, rather than each being an independent
    // draw across the whole band.
    // -------------------------------------------------------------
    float TargetCps() {
        int lo = m_cpsMin, hi = m_cpsMax;
        if (lo < 1) lo = 1;
        if (hi < lo) hi = lo;

        // Ornstein-Uhlenbeck-ish: pull toward centre, nudge randomly,
        // bound. Correlated noise, which plain per-click randomness
        // cannot fake.
        m_drift += -0.5 * m_drift + Gauss(0.0, 0.35);
        if (m_drift >  1.0) m_drift =  1.0;
        if (m_drift < -1.0) m_drift = -1.0;

        float mid  = (lo + hi) * 0.5f;
        float half = (hi - lo) * 0.5f;
        float cps  = mid + (float)m_drift * half;

        if (cps < 1.0f)     cps = 1.0f;
        if (cps > kMaxCps)  cps = kMaxCps;
        return cps;
    }

    // -------------------------------------------------------------
    // Ticks until the next click. Carries its fractional remainder
    // through the caller, so a non-integer tick target averages out.
    // Never returns below 1.0: two clicks in one tick is the one
    // interval a hand cannot make.
    // -------------------------------------------------------------
    float RollIntervalTicks() {
        // The fast half of a butterfly pair: exactly one tick after
        // its partner, then the recovery gap comes next.
        if (m_burstLeft > 0) {
            m_burstLeft--;
            return 1.0f;
        }

        float cps  = TargetCps();
        float base = 20.0f / cps;      // 20 TPS -> ticks per click

        if (m_randomize && m_variance > 0.0f) {
            double sigma = base * (double)(m_variance * 0.01f);
            base = (float)Gauss(base, sigma);
        }

        if (m_fatigue && Roll(m_fatigueChance)) {
            base *= std::uniform_real_distribution<float>(1.4f, 2.2f)(m_rng);
        }

        // Butterfly: line up a partner one tick out. Charged after
        // this gap, so pairs sit among the normal spacing rather
        // than replacing it.
        if (m_pattern == 1 && m_burstLeft == 0 && Roll(m_burstChance)) {
            m_burstLeft = 1;
        }

        if (base < 1.0f) base = 1.0f;
        return base;
    }

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;

        if (ClassResolver::playerController) {
            m_fHittingBlock = JvmtiUtil::FindField(env,
                ClassResolver::playerController,
                { "field_78778_j", "isHittingBlock" });
        }

        // Held-item lookups go on the LIVE player's class, because
        // Lunar's hierarchy does not always match vanilla.
        jobject player = Minecraft::GetPlayer(env);
        if (player) {
            jclass pc = env->GetObjectClass(player);
            m_fInventory = JvmtiUtil::FindField(env, pc,
                { "field_71071_by", "inventory" });
            env->DeleteLocalRef(pc);

            if (m_fInventory) {
                jobject inv = env->GetObjectField(player, m_fInventory);
                if (inv) {
                    jclass ic = env->GetObjectClass(inv);
                    m_mCurItem = JvmtiUtil::FindMethod(env, ic,
                        { "func_70448_g", "getCurrentItem" }, 0);
                    env->DeleteLocalRef(ic);
                    env->DeleteLocalRef(inv);
                }
            }
            // A player is required to resolve the item path, so only
            // latch resolved once we had one.
            m_resolved = true;
        }
    }

    // Left button on a block that is actively breaking. A queued
    // click there restarts the swing and resets the break, so a
    // spammed button mines slower or not at all. Fail-open: if the
    // field is unresolved we cannot tell, so we do not gate.
    bool BreakingBlock(JNIEnv* env) {
        if (!m_breakSafe) return false;
        if (m_button != 0) return false;
        if (!m_fHittingBlock) return false;

        jobject pc = Minecraft::GetPlayerController(env);
        if (!pc) return false;
        bool hitting = env->GetBooleanField(pc, m_fHittingBlock) != 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return hitting;
    }

    // Sword or axe in hand. Fail-open: if the item cannot be read we
    // allow the click rather than silently refusing to work.
    bool HeldIsWeapon(JNIEnv* env, jobject player) {
        if (!m_weaponsOnly) return true;
        if (!m_fInventory || !m_mCurItem) return true;

        jobject inv = env->GetObjectField(player, m_fInventory);
        if (!inv) return true;

        jobject stack = env->CallObjectMethod(inv, m_mCurItem);
        env->DeleteLocalRef(inv);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
        if (!stack) return false;   // empty hand is not a weapon

        if (!m_mStackName) {
            jclass sc = env->GetObjectClass(stack);
            m_mStackName = JvmtiUtil::FindMethod(env, sc,
                { "func_77977_a", "getUnlocalizedName" }, 0);
            env->DeleteLocalRef(sc);
        }
        if (!m_mStackName) { env->DeleteLocalRef(stack); return true; }

        jstring js = (jstring)env->CallObjectMethod(stack, m_mStackName);
        env->DeleteLocalRef(stack);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
        if (!js) return true;

        bool weapon = false;
        const char* raw = env->GetStringUTFChars(js, nullptr);
        if (raw) {
            char low[160];
            size_t n = std::strlen(raw);
            if (n > sizeof(low) - 1) n = sizeof(low) - 1;
            for (size_t i = 0; i < n; i++)
                low[i] = (char)std::tolower((unsigned char)raw[i]);
            low[n] = '\0';
            weapon = std::strstr(low, "sword") || std::strstr(low, "axe");
            env->ReleaseStringUTFChars(js, raw);
        }
        env->DeleteLocalRef(js);
        return weapon;
    }

    void NoteFire() {
        auto now = Clock::now();
        m_fires.push_back(now);
        // Keep a one-second window for the live figure.
        while (!m_fires.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_fires.front()).count();
            if (age <= 1000) break;
            m_fires.pop_front();
        }
    }

    float LiveCps() const {
        if (m_fires.size() < 2) return 0.0f;
        auto span = std::chrono::duration_cast<std::chrono::milliseconds>(
            m_fires.back() - m_fires.front()).count();
        if (span <= 0) return 0.0f;
        return (float)(m_fires.size() - 1) * 1000.0f / (float)span;
    }

public:
    AutoClicker()
        : Module("AutoClicker", "Clicks the held button, humanised",
                 ModuleCategory::COMBAT, 0)
    {
        BindMode("Button", &m_button, kButtons, 2,
                 "Which mouse button it clicks for you");

        Bind("CPS Min", &m_cpsMin, 1, 20,
             "Slowest it will average");
        Bind("CPS Max", &m_cpsMax, 1, 20,
             "Fastest it will average. 20 is the hard ceiling: one click "
             "per tick is all the server can see.");

        // ---- Pattern ----
        BindMode("Pattern", &m_pattern, kPatterns, 2,
                 "Jitter is single clicks. Butterfly adds the occasional "
                 "pair a tick apart, the structure some anticheats expect "
                 "once you are clicking fast.")
            .Advanced();

        Bind("Randomize", &m_randomize,
             "Gaussian spread and slow drift on the rate. Off is a flat "
             "rate for servers with no anticheat.")
            .Advanced();

        Bind("Variance", &m_variance, 5.0f, 45.0f, "%.0f%%",
             "How wide the spread around your rate is. Under about 8% the "
             "stream reads as a machine.")
            .When("Randomize", 1).Advanced();

        Bind("Fatigue", &m_fatigue,
             "The occasional slower gap, the way a hand tires")
            .Advanced();

        Bind("Fatigue Chance", &m_fatigueChance, 0.5f, 12.0f, "%.1f%%")
            .When("Fatigue", 1).Advanced();

        Bind("Butterfly Chance", &m_burstChance, 2.0f, 30.0f, "%.0f%%",
             "How often a pair fires")
            .When("Pattern", 1).Advanced();

        // ---- Gating ----
        Bind("Do Not Break Mining", &m_breakSafe,
             "Pause while you are breaking a block, so holding left-click "
             "mines normally instead of the clicker resetting it. Left "
             "button only.")
            .Advanced();

        Bind("Weapons Only", &m_weaponsOnly,
             "Only click with a sword or axe in hand, so it never eats food "
             "or places a block by accident")
            .Advanced();

        Bind("Only With A Target", &m_requireTarget,
             "Only click when someone is in reach, never at thin air")
            .Advanced();

        Bind("Target Range", &m_targetRange, 2.0f, 8.0f, "%.1f",
             "How close something has to be to count")
            .When("Only With A Target", 1).Advanced();
    }

    void OnEnable(JNIEnv*) override {
        m_nextInTicks = 0.0f;
        m_burstLeft   = 0;
        m_drift       = 0.0;
        m_wasHolding  = false;
        m_fires.clear();
        m_why = "waiting for the button";
    }

    void OnDisable(JNIEnv*) override {
        m_burstLeft  = 0;
        m_wasHolding = false;
        m_fires.clear();
        m_why = "off";
        m_status[0] = '\0';
    }

    void OnReset(JNIEnv*) override {
        m_nextInTicks = 0.0f;
        m_burstLeft   = 0;
        m_drift       = 0.0;
        m_wasHolding  = false;
        m_fires.clear();
        m_why = "waiting for the button";
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);

        // A hand-edited config can invert the pair; a slider cannot.
        if (m_cpsMin > m_cpsMax) m_cpsMin = m_cpsMax;

        if (!KeyBinds::Init(env) || !KeyBinds::HasClickQueue()) {
            m_why = "click queue unavailable";
            m_status[0] = '\0';
            return;
        }

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { m_why = "no player"; m_status[0] = '\0'; return; }

        // Vanilla screen open: runTick does not drain pressTime, so
        // a click now would sit and fire on close. (The Phantom menu
        // is handled by the manager, which does not tick us at all
        // while it is up.)
        if (Minecraft::IsInGui(env)) {
            m_wasHolding = false;
            m_why = "menu";
            m_status[0] = '\0';
            return;
        }

        // The button, but only while the game actually has focus:
        // a bare GetAsyncKeyState would count a click in another app.
        bool holding = Focus::KeyHeld(VkForButton());
        if (!holding) {
            m_wasHolding = false;
            m_why = "hold the button to start";
            snprintf(m_status, sizeof(m_status), "%s", m_why);
            return;
        }

        // Fresh press: fire promptly on the first eligible tick, and
        // start a clean drift curve.
        if (!m_wasHolding) {
            m_wasHolding  = true;
            m_nextInTicks = 0.0f;
            m_burstLeft   = 0;
            m_drift       = 0.0;
        }

        // ---- Gates ----
        if (BreakingBlock(env)) { m_why = "mining"; m_nextInTicks = 0.0f; return; }
        if (!HeldIsWeapon(env, player)) { m_why = "no weapon"; return; }
        if (m_requireTarget
            && CombatState::TargetDist() > (double)m_targetRange) {
            m_why = "nothing in range";
            return;
        }

        // ---- The one decision per tick ----
        // Every tick spends one tick of the countdown, INCLUDING the
        // one we fire on. Crediting the fire tick for free (only
        // decrementing on non-firing ticks) stretches the real
        // period to 1 + interval, so a 12 CPS request came out around
        // 7. Decrement first, then fire when the debt is paid and add
        // the next interval; the average rate now matches the target.
        // The interval floor of 1.0 plus a single queued click keeps
        // this to at most one click per tick regardless.
        m_nextInTicks -= 1.0f;
        if (m_nextInTicks <= 0.0f) {
            int done = (m_button == 1) ? KeyBinds::QueueUse(env, 1)
                                       : KeyBinds::QueueAttack(env, 1);
            if (done > 0) NoteFire();
            m_nextInTicks += RollIntervalTicks();
        }

        m_why = "clicking";
        snprintf(m_status, sizeof(m_status), "%.1f CPS", LiveCps());
    }

    const char* StatusLine() const override {
        return m_status[0] ? m_status : nullptr;
    }

    NoticeLevel Notice(const char** text) const override {
        if (!KeyBinds::HasClickQueue()) {
            *text = "The click counter could not be found in this build of "
                    "the game, so clicks cannot reach it.";
            return NoticeLevel::Danger;
        }
        if (m_button == 0 && m_breakSafe && !m_fHittingBlock) {
            *text = "The mining-state field could not be read, so the clicker "
                    "cannot tell when you are breaking a block and may reset "
                    "it. Everything else works.";
            return NoticeLevel::Info;
        }
        if (CeilingClampedMax() >= (float)kNoisyCps) {
            snprintf(m_notice, sizeof(m_notice),
                     "Averaging toward %d+ CPS is butterfly territory: "
                     "reachable by hand, but people notice on a recording.",
                     kNoisyCps);
            *text = m_notice;
            return NoticeLevel::Warning;
        }
        return NoticeLevel::None;
    }
};
