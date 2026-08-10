#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../input/focus.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <random>
#include <cmath>

// =================================================================
// AutoClicker
// =================================================================
// Clicks for you while you hold the button. Built from one fact
// about 1.8, which decides the whole shape of it:
//
//     Minecraft.runTick():
//         while (gameSettings.keyBindAttack.isPressed()) clickMouse();
//     KeyBinding.isPressed():
//         if (pressTime == 0) return false;
//         --pressTime; return true;
//
// clickMouse() is drained at the TICK boundary, 20 times a second.
// So whatever the server ends up seeing is fully described by "how
// many clicks landed in which tick". Sub-tick timing never reaches
// the wire. The previous clicker ran a 1ms thread to shape the gap
// between physical clicks precisely, which bought nothing the
// server could see and cost a second thread poking shared state. So
// this one lives entirely on the client tick, where JNI is legal
// and there is nothing to race.
//
// THE ONE HARD RULE: AT MOST ONE CLICK PER TICK.
// Two clicks in a single tick are two swings with zero time between
// them, a sub-20ms interval no hand can make and the clearest
// machine signature there is. The ceiling is therefore ~20 CPS,
// which is also where real anticheats start flagging sustained
// rates, so the Max slider stops there and does not pretend
// otherwise.
//
// -----------------------------------------------------------------
// HOW THE RATE IS SHAPED
// -----------------------------------------------------------------
// A carry accumulator, the same idea as the rotation grid fix. Each
// tick adds targetCPS/20 to a running progress value; when it
// crosses a threshold we fire once, subtract the threshold and
// carry the remainder into the next tick. Over any stretch the mean
// rate is exactly targetCPS, and the threshold is the single knob
// we shape for humanisation without touching that mean:
//
//   Randomize   threshold drawn from a Gaussian around 1.0, so the
//               interval histogram is a bell rather than the flat
//               line a uniform delay produces. Target CPS also
//               drifts slowly between Min and Max, so neighbouring
//               intervals are correlated the way a hand speeds up
//               and eases off, instead of each being independent.
//
//   Fatigue     rare threshold spikes: the occasional slow patch a
//               tiring hand produces and a loop never does.
//
//   Butterfly   thresholds alternate low then high, so clicks land
//               as close PAIRS in neighbouring ticks with a longer
//               gap between pairs, which is what real two-finger
//               butterfly clicking looks like. The low and high
//               average back to 1.0, so the CPS is unchanged; only
//               the shape differs. Some anticheats expect double
//               clicks above a certain rate and this provides them.
// =================================================================

class AutoClicker : public Module {
private:
    static constexpr const char* kButtons[]  = { "Left (attack)", "Right (use)" };
    static constexpr const char* kPatterns[] = { "Jitter", "Butterfly" };

    // Above this the rate is reachable by hand but people notice on
    // a recording, so the notice warns past it.
    static constexpr int kButterflyCPS = 16;
    // One click per tick means 20 TPS is the hard ceiling.
    static constexpr int kCeilingCPS = 20;

    // ---- Core ----
    int m_button = 0;            // 0 attack, 1 use item
    int m_cpsMin = 10;
    int m_cpsMax = 13;

    // ---- Advanced ----
    int   m_pattern       = 0;   // 0 jitter, 1 butterfly
    bool  m_randomize     = true;
    float m_variance      = 20.0f;   // percent, spread of the threshold
    bool  m_fatigue       = true;
    float m_fatigueChance = 4.0f;
    bool  m_breakSafe     = true;    // do not fight block breaking
    bool  m_onlyTarget    = false;
    float m_targetRange   = 4.0f;
    bool  m_weaponsOnly    = false;

    // ---- Scheduler state ----
    double m_progress   = 0.0;   // accumulated clicks-worth of ticks
    double m_threshold  = 1.0;   // progress needed for the next click
    double m_targetCps  = 11.0;  // drifts between min and max
    bool   m_pairSecond = false; // butterfly: the low half is pending

    // ---- Readout ----
    unsigned long long m_tick = 0;
    unsigned long long m_lastFireTick = 0;
    float m_liveCps = 0.0f;
    long long m_fired = 0;
    const char* m_why = "idle";
    mutable char m_status[48] = {};
    mutable char m_notice[176] = {};

    // ---- JNI ----
    jfieldID  m_fHittingBlock = nullptr;   // PlayerControllerMP.isHittingBlock
    jfieldID  m_fInventory    = nullptr;   // EntityPlayer.inventory
    jmethodID m_mCurrentItem  = nullptr;   // InventoryPlayer.getCurrentItem
    jmethodID m_mStackName    = nullptr;   // ItemStack.getUnlocalizedName
    bool m_resolved  = false;
    bool m_itemUsable = false;

    std::mt19937 m_rng{ std::random_device{}() };

    bool Roll(float pct) {
        return std::uniform_real_distribution<float>(0.f, 100.f)(m_rng) < pct;
    }

    // Box-Muller, one draw. Kept explicit so the shape is identical
    // across compilers rather than trusting the library's normal.
    double Gauss(double mean, double sigma) {
        std::uniform_real_distribution<double> u(1e-9, 1.0);
        double u1 = u(m_rng), u2 = u(m_rng);
        double z = std::sqrt(-2.0 * std::log(u1)) *
                   std::cos(6.283185307179586 * u2);
        return mean + sigma * z;
    }

    // -------------------------------------------------------------
    // Resolution. Everything here is optional: an unresolved field
    // only turns off the feature that needs it, never the clicker.
    // -------------------------------------------------------------
    void Resolve(JNIEnv* env) {
        if (m_resolved) return;

        if (ClassResolver::playerController) {
            m_fHittingBlock = JvmtiUtil::FindField(env,
                ClassResolver::playerController,
                { "field_78778_j", "isHittingBlock" });
        }

        // Held-item lookup, resolved on the LIVE player's class the
        // way Auto Blockhit did, because Lunar's hierarchy does not
        // always match vanilla.
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
                    m_mCurrentItem = JvmtiUtil::FindMethod(env, ic,
                        { "func_70448_g", "getCurrentItem" }, 0);
                    env->DeleteLocalRef(ic);
                    env->DeleteLocalRef(inv);
                }
            }
            m_itemUsable = (m_fInventory != nullptr && m_mCurrentItem != nullptr);
            m_resolved = true;
        }
        // If the player was null we leave m_resolved false and try
        // again next tick; the field ids are worthless without it.
    }

    // Is the player mid-swing on a block right now? Only the left
    // button mines, and an unresolved field means we cannot tell, so
    // in both of those cases we do NOT gate: fail open.
    bool BreakingBlock(JNIEnv* env) {
        if (!m_breakSafe || m_button != 0 || !m_fHittingBlock) return false;
        jobject pc = Minecraft::GetPlayerController(env);
        if (!pc) return false;
        bool hitting = env->GetBooleanField(pc, m_fHittingBlock) != 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return hitting;
    }

    // Sword or axe in hand. Unlocalised names are literal source
    // strings and survive obfuscation, so a substring test on
    // getUnlocalizedName is more dependable than chasing ItemSword.
    // Fail-open: if we cannot read the item we allow the click.
    bool HoldingWeapon(JNIEnv* env, jobject player) {
        if (!m_weaponsOnly) return true;
        if (!m_itemUsable)  return true;   // cannot tell, do not block

        jobject inv = env->GetObjectField(player, m_fInventory);
        if (!inv) return true;

        jobject stack = env->CallObjectMethod(inv, m_mCurrentItem);
        env->DeleteLocalRef(inv);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
        if (!stack) return false;          // empty hand is not a weapon

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

    // The threshold for the NEXT click, in units of "one click's
    // worth of progress". Mean is 1.0 so the configured CPS is
    // preserved; the spread is where the humanisation lives.
    double NextThreshold() {
        // ---- Butterfly: alternate a short gap and a long one ----
        // The pair (low, high) averages to 1.0, so two clicks land
        // close together and then a wider gap follows, which is the
        // real two-finger rhythm rather than an even stream.
        if (m_pattern == 1) {
            m_pairSecond = !m_pairSecond;
            double base = m_pairSecond
                        ? 0.45   // the quick second of the pair
                        : 1.55;  // the rest between pairs
            if (m_randomize) {
                double j = base * (double)(m_variance * 0.01f);
                base += Gauss(0.0, j * 0.5);
            }
            if (base < 0.30) base = 0.30;
            return base;
        }

        // ---- Jitter: a Gaussian around one click's worth ----
        double t = 1.0;
        if (m_randomize) {
            double sigma = (double)(m_variance * 0.01f);   // ~0.20
            t = Gauss(1.0, sigma);
        }

        // Fatigue: the occasional slow patch. Multiplicative so it
        // reads as easing off rather than a single dropped click.
        if (m_fatigue && Roll(m_fatigueChance)) {
            std::uniform_real_distribution<double> f(1.6, 2.8);
            t *= f(m_rng);
        }

        // A threshold below half a click would let two clicks land
        // in consecutive ticks unprompted; keep jitter from doing
        // butterfly's job by accident.
        if (t < 0.55) t = 0.55;
        if (t > 3.0)  t = 3.0;
        return t;
    }

    // Slowly walk the target rate between Min and Max so the mean
    // itself wanders rather than sitting on one value. A small step
    // per tick keeps neighbouring intervals related.
    void DriftTargetCps() {
        int lo = m_cpsMin, hi = m_cpsMax;
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        if (lo < 1) lo = 1;
        if (hi > kCeilingCPS) hi = kCeilingCPS;

        if (!m_randomize) {
            // Steady: sit exactly in the middle of the band.
            m_targetCps = 0.5 * (lo + hi);
            return;
        }

        double span = (double)(hi - lo);
        double step = (span > 0.0 ? span : 1.0) * 0.08;
        m_targetCps += Gauss(0.0, step);
        if (m_targetCps < lo) m_targetCps = lo;
        if (m_targetCps > hi) m_targetCps = hi;
    }

    void ResetSchedule() {
        m_progress   = 0.0;
        m_pairSecond = false;
        int lo = m_cpsMin, hi = m_cpsMax;
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        m_targetCps = 0.5 * (lo + hi);
        m_threshold = 1.0;
    }

public:
    AutoClicker()
        : Module("AutoClicker", "Clicks while you hold the button",
                 ModuleCategory::COMBAT, 0)
    {
        BindMode("Button", &m_button, kButtons, 2,
                 "Which mouse button it clicks for you");

        Bind("CPS Min", &m_cpsMin, 1, kCeilingCPS,
             "Slowest end of the target rate");

        Bind("CPS Max", &m_cpsMax, 1, kCeilingCPS,
             "Fastest end of the target rate. One click per tick caps this "
             "at 20, which is also where servers start flagging.");

        // ---- Advanced ----
        BindMode("Pattern", &m_pattern, kPatterns, 2,
                 "Jitter spaces single clicks out. Butterfly lands them in "
                 "close pairs like two fingers, which some anticheats expect "
                 "above a certain rate.")
            .Advanced();

        Bind("Randomize", &m_randomize,
             "Gaussian intervals and a drifting rate, so the stream is not a "
             "flat machine histogram. Turn off only on a server you know "
             "does not check.")
            .Advanced();

        Bind("Variance", &m_variance, 5.0f, 45.0f, "%.0f%%",
             "How wide the spread around your rate is. Under about 8% reads "
             "as a machine.")
            .When("Randomize", 1).Advanced();

        Bind("Fatigue", &m_fatigue,
             "The occasional slow patch a tiring hand produces")
            .When("Randomize", 1).Advanced();

        Bind("Fatigue Chance", &m_fatigueChance, 0.5f, 12.0f, "%.1f%%")
            .When("Fatigue", 1).Advanced();

        Bind("Break-Safe", &m_breakSafe,
             "Pause while you are breaking a block, so holding left-click "
             "mines normally instead of the clicker resetting it. Left "
             "button only.")
            .Advanced();

        Bind("Only With Target", &m_onlyTarget,
             "Only click when someone is in reach, not at thin air")
            .Advanced();

        Bind("Target Range", &m_targetRange, 2.0f, 6.0f, "%.1f",
             "How close a target has to be to count")
            .When("Only With Target", 1).Advanced();

        Bind("Weapons Only", &m_weaponsOnly,
             "Only click with a sword or axe in hand, so it does not eat "
             "food or place blocks")
            .Advanced();
    }

    void OnEnable(JNIEnv*) override {
        ResetSchedule();
        m_why = "hold the button to start";
    }

    void OnDisable(JNIEnv*) override {
        ResetSchedule();
        m_liveCps = 0.0f;
        m_why = "off";
        m_status[0] = '\0';
    }

    // A world change or respawn is a fresh start: forget the rhythm
    // so the first click in the next world is not timed against the
    // last one.
    void OnReset(JNIEnv*) override {
        ResetSchedule();
        m_liveCps = 0.0f;
        m_why = "hold the button to start";
    }

    void OnTick(JNIEnv* env) override {
        Resolve(env);
        m_tick++;

        // Sanitise a hand-edited config: a slider cannot invert the
        // band but a file can.
        if (m_cpsMin > m_cpsMax) { int t = m_cpsMin; m_cpsMin = m_cpsMax; m_cpsMax = t; }

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { m_progress = 0.0; m_why = "no player"; m_status[0] = '\0'; return; }

        // A screen is open (including the Phantom menu): stand down
        // and forget the accumulator, or it would dump a burst the
        // moment the screen closes.
        if (Minecraft::IsInGui(env)) {
            m_progress = 0.0;
            m_why = "menu";
            m_status[0] = '\0';
            return;
        }

        if (!KeyBinds::HasClickQueue()) {
            m_why = "click queue unresolved";
            m_status[0] = '\0';
            return;
        }

        // The button, but only while the game actually has focus, so
        // a click in another window does not drive it.
        int vk = (m_button == 1) ? VK_RBUTTON : VK_LBUTTON;
        if (!Focus::KeyHeld(vk)) {
            m_progress = 0.0;
            m_pairSecond = false;
            m_why = "hold the button to start";
            m_status[0] = '\0';
            return;
        }

        // ---- Gates that permit holding but forbid clicking ----
        if (BreakingBlock(env)) {
            m_progress = 0.0;
            m_why = "mining";
            snprintf(m_status, sizeof(m_status), "mining");
            return;
        }

        if (m_onlyTarget && CombatState::TargetDist() > (double)m_targetRange) {
            m_progress = 0.0;
            m_why = "nothing in range";
            snprintf(m_status, sizeof(m_status), "waiting");
            return;
        }

        if (!HoldingWeapon(env, player)) {
            m_progress = 0.0;
            m_why = "no weapon";
            snprintf(m_status, sizeof(m_status), "no weapon");
            return;
        }

        // ---- Accumulate and maybe fire ----
        DriftTargetCps();
        m_progress += m_targetCps / 20.0;   // clicks-worth of this tick

        // At most ONE click per tick, whatever the arithmetic says.
        // The remainder carries, so the mean rate is still exact.
        if (m_progress >= m_threshold) {
            int done = (m_button == 1)
                     ? KeyBinds::QueueUse(env, 1)
                     : KeyBinds::QueueAttack(env, 1);
            if (done > 0) {
                m_progress -= m_threshold;
                m_threshold = NextThreshold();
                m_fired++;

                // Live rate from the tick gap, smoothed so the row
                // is readable rather than jumping every click.
                if (m_lastFireTick > 0) {
                    unsigned long long gap = m_tick - m_lastFireTick;
                    if (gap >= 1) {
                        float inst = 20.0f / (float)gap;
                        m_liveCps += (inst - m_liveCps) * 0.25f;
                    }
                }
                m_lastFireTick = m_tick;
                m_why = "clicking";
            }
            // If the game refused the click (queue full), leave the
            // progress alone and try again next tick.
        }

        if (m_liveCps > 0.1f)
            snprintf(m_status, sizeof(m_status), "%.1f CPS", m_liveCps);
        else
            snprintf(m_status, sizeof(m_status), "%s", m_why);
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
        if (m_breakSafe && m_button == 0 && !m_fHittingBlock) {
            *text = "The mining-state field could not be read, so the clicker "
                    "cannot tell when you are breaking a block and Break-Safe "
                    "does nothing. Everything else works.";
            return NoticeLevel::Info;
        }
        if (m_cpsMax > kButterflyCPS) {
            snprintf(m_notice, sizeof(m_notice),
                     "Above %d CPS is reachable by hand but stands out on a "
                     "recording. Butterfly reads more naturally up there than "
                     "Jitter.", kButterflyCPS);
            *text = m_notice;
            return NoticeLevel::Warning;
        }
        return NoticeLevel::None;
    }
};
