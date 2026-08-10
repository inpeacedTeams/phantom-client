#pragma once
#include "../module.h"
#include "../../mc/minecraft.h"
#include "../../mc/keybinds.h"
#include "../../mc/combat_state.h"
#include "../../input/click_scheduler.h"
#include "../../jni/class_resolver.h"
#include "../../jni/jvmti_util.h"
#include <cstdio>

// =================================================================
// AutoClicker
// =================================================================
// Clicks while you hold the button. That is the whole feature, and
// the previous version got it wrong in three ways worth naming.
//
//   It never clicked at all. Clicks went out through SendInput,
//   which cannot inject a press for a button Windows already
//   considers held. Clicks now go into KeyBinding.pressTime, the
//   counter a real click increments.
//
//   It reached back into this object from the timer thread while
//   the render thread was drawing the same fields. Timing now lives
//   entirely in the engine and the module only publishes a POD.
//
//   It polled the button on the 20 TPS tick, so the first click was
//   late and clicks kept landing for up to 50ms after release. The
//   engine watches the button itself at 1ms.
//
// WHAT THIS MODULE STILL DOES
// Decides whether clicking is ALLOWED. Being in a world, not in a
// menu, optionally only with something in front of you, and NOT
// while breaking a block. The button itself is the engine's
// business.
//
// -----------------------------------------------------------------
// WHY IT STANDS DOWN WHILE MINING
// -----------------------------------------------------------------
// A queued click is one clickMouse() call. On an entity that is an
// attack. On a block it re-starts the swing and RESETS the break
// progress, so a spammed left button can never break anything:
// mining in 1.8 is driven by HOLDING the key, not by clicks. If the
// clicker kept firing while you held attack on a block you would
// mine at a crawl or not at all, which is exactly the "it fights my
// mining" complaint.
//
// PlayerControllerMP.isHittingBlock is true for precisely the window
// the player is breaking a block. While it is set the clicker does
// not arm; the physical hold mines normally and the clicker resumes
// the instant the block breaks or the crosshair leaves it. Hitting
// an entity or swinging at air never sets it, so combat is
// untouched.
// =================================================================

class AutoClicker : public Module {
private:
    static constexpr const char* kButtons[] = { "Left (attack)", "Right (use)" };

    // ---- Core ----
    float m_cps = 12.0f;
    int   m_button = 0;          // 0 attack, 1 use item

    // ---- Gating ----
    bool  m_requireTarget = false;
    float m_targetRange   = 4.0f;
    bool  m_blockInGui    = true;
    bool  m_pauseMining   = true;   // do not fight block breaking

    // ---- Humanisation ----
    float m_variance     = 18.0f;
    bool  m_drift        = true;
    float m_driftAmount  = 12.0f;
    bool  m_fumbles      = true;
    float m_fumbleChance = 3.0f;
    bool  m_bursts       = true;
    float m_burstChance  = 4.0f;
    int   m_floorMs      = 22;

    // ---- Readout ----
    const char* m_why = "idle";
    mutable char m_status[48] = {};
    mutable char m_notice[160] = {};

    // ---- JNI: mining state ----
    jfieldID m_fHittingBlock = nullptr;
    bool m_resolved = false;

    // Above this a click stream stops looking like a hand and starts
    // looking like a loop, whatever the humanisation does.
    static constexpr float kButterflyCPS = 16.0f;

    ClickerConfig BuildConfig() const {
        ClickerConfig c;
        c.cps          = m_cps;
        c.variance     = m_variance;
        c.drift        = m_drift;
        c.driftAmount  = m_driftAmount;
        c.fumbles      = m_fumbles;
        c.fumbleChance = m_fumbleChance;
        c.bursts       = m_bursts;
        c.burstChance  = m_burstChance;
        c.floorMs      = m_floorMs;
        c.rightButton  = (m_button == 1);
        return c;
    }

    // The fastest rate this configuration can actually reach. Asking
    // for 30 CPS with a 22ms floor gets you 45, and the panel should
    // say so rather than quietly lying.
    float CeilingCPS() const {
        return m_floorMs > 0 ? 1000.0f / (float)m_floorMs : 99.0f;
    }

    void Resolve(JNIEnv* env) {
        if (m_resolved) return;
        if (ClassResolver::playerController) {
            m_fHittingBlock = JvmtiUtil::FindField(env,
                ClassResolver::playerController,
                { "field_78778_j", "isHittingBlock" });
        }
        m_resolved = true;
    }

    // Is the player mid-swing on a block right now? Only meaningful
    // for the LEFT button; right-click use has nothing to do with
    // breaking. Fail-open: an unresolved field means we cannot tell,
    // so do not block clicking.
    bool BreakingBlock(JNIEnv* env) {
        if (!m_pauseMining) return false;
        if (m_button != 0) return false;         // only the attack button mines
        if (!m_fHittingBlock) return false;      // cannot tell, do not gate

        jobject pc = Minecraft::GetPlayerController(env);
        if (!pc) return false;
        bool hitting = env->GetBooleanField(pc, m_fHittingBlock) != 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return hitting;
    }

public:
    AutoClicker()
        : Module("AutoClicker", "Clicks while you hold the button",
                 ModuleCategory::COMBAT, 0)
    {
        BindMode("Button", &m_button, kButtons, 2,
                 "Which mouse button it clicks for you");

        Bind("CPS", &m_cps, 1.0f, 22.0f, "%.0f",
             "Clicks per second, on average");

        Bind("Variance", &m_variance, 2.0f, 45.0f, "%.0f%%",
             "Spread around your CPS. Under about 8% the stream reads as "
             "a machine.")
            .Advanced();

        Bind("Drift", &m_drift,
             "Slow wander, so neighbouring clicks are related the way a real "
             "hand speeds up and eases off")
            .Advanced();

        Bind("Drift Amount", &m_driftAmount, 2.0f, 30.0f, "%.0f%%",
             "How far the wander goes")
            .Advanced();

        Bind("Fumbles", &m_fumbles,
             "The occasional missed click. Hands produce them, loops do not.")
            .Advanced();

        Bind("Fumble Chance", &m_fumbleChance, 0.5f, 12.0f, "%.1f%%")
            .Advanced();

        Bind("Bursts", &m_bursts,
             "The occasional pair that lands faster than the rest")
            .Advanced();

        Bind("Burst Chance", &m_burstChance, 0.5f, 12.0f, "%.1f%%")
            .Advanced();

        Bind("Only With A Target", &m_requireTarget,
             "Do not click at thin air")
            .Advanced();

        Bind("Target Range", &m_targetRange, 2.0f, 8.0f, "%.1f",
             "How close something has to be to count")
            .Advanced();

        Bind("Stop In Menus", &m_blockInGui,
             "Chests and inventories are not places to be clicking")
            .Advanced();

        Bind("Do Not Break Mining", &m_pauseMining,
             "Pause while you are breaking a block, so holding left-click "
             "mines normally instead of the clicker resetting it. Left "
             "button only.")
            .Advanced();

        Bind("Floor", &m_floorMs, 18, 60,
             "Hard minimum milliseconds between clicks, shared with Hit "
             "Select so the two cannot stack. Nothing under 20 is "
             "physically reachable.")
            .Advanced();
    }

    void OnEnable(JNIEnv*) override {
        ClickScheduler::SetConfig(BuildConfig());
        m_why = "waiting for the button";
    }

    void OnDisable(JNIEnv* env) override {
        ClickScheduler::SetArmed(false);
        ClickScheduler::ClearPending();
        // Anything the game has not consumed yet would fire on the
        // next tick, after the module is already off.
        if (env) KeyBinds::ClearClickQueue(env);
        m_why = "off";
        m_status[0] = '\0';
    }

    // A world change while the button is held would otherwise carry
    // a queue of clicks into the loading screen.
    void OnReset(JNIEnv* env) override {
        ClickScheduler::SetArmed(false);
        ClickScheduler::ClearPending();
        if (env) KeyBinds::ClearClickQueue(env);
        m_why = "waiting for the button";
    }

    // Only decides whether clicking is permitted. The engine handles
    // the button and the timing.
    void OnTick(JNIEnv* env) override {
        Resolve(env);
        ClickScheduler::SetConfig(BuildConfig());

        jobject player = Minecraft::GetPlayer(env);
        if (!player) {
            ClickScheduler::SetArmed(false);
            m_why = "no player";
            m_status[0] = '\0';
            return;
        }

        if (m_blockInGui && Minecraft::IsInGui(env)) {
            // runTick does not drain pressTime while a screen is
            // open, so a backlog would fire all at once on close.
            ClickScheduler::SetArmed(false);
            KeyBinds::ClearClickQueue(env);
            m_why = "menu";
            m_status[0] = '\0';
            return;
        }

        bool allowed = true;
        const char* why = "ready";

        // Breaking a block: hand the button back to vanilla mining.
        // A queued click here just resets the break, so the clicker
        // would stop you mining rather than help.
        if (BreakingBlock(env)) {
            allowed = false;
            why = "mining";
        }

        if (allowed && m_requireTarget
            && CombatState::TargetDist() > (double)m_targetRange) {
            allowed = false;
            why = "nothing in range";
        }

        if (!KeyBinds::HasClickQueue()) {
            allowed = false;
            why = "click queue unresolved";
        }

        ClickScheduler::SetArmed(allowed);

        if (!allowed)                         m_why = why;
        else if (ClickScheduler::IsHolding()) m_why = "clicking";
        else                                  m_why = "hold the button to start";

        // While mining, the pending queue must not sit and then fire
        // the instant the block breaks, so drop it on the same tick
        // we stand down.
        if (!allowed) ClickScheduler::ClearPending();

        // The live figure is the only honest answer to "is this
        // working", so it goes on the collapsed row rather than
        // being buried in a panel nobody has open mid-fight.
        if (ClickScheduler::IsClicking()) {
            snprintf(m_status, sizeof(m_status), "%.1f CPS  \xC2\xB1%.0f ms",
                     ClickScheduler::LiveCPS(), ClickScheduler::LiveStdDev());
        } else {
            snprintf(m_status, sizeof(m_status), "%s", m_why);
        }
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
        if (!ClickScheduler::IsRunning()) {
            *text = "The click timer is not running, so nothing will be sent.";
            return NoticeLevel::Danger;
        }

        float ceiling = CeilingCPS();
        if (m_cps > ceiling) {
            snprintf(m_notice, sizeof(m_notice),
                     "The %d ms floor caps this at %.0f CPS, so the slider "
                     "above that does nothing.", m_floorMs, ceiling);
            *text = m_notice;
            return NoticeLevel::Warning;
        }

        long long dropped = ClickScheduler::Dropped();
        if (dropped > 30) {
            snprintf(m_notice, sizeof(m_notice),
                     "%lld clicks were dropped: this is more than the game "
                     "can consume. Lower the CPS.", dropped);
            *text = m_notice;
            return NoticeLevel::Warning;
        }

        if (m_button == 0 && m_pauseMining && !m_fHittingBlock) {
            *text = "The mining-state field could not be read, so the clicker "
                    "cannot tell when you are breaking a block and may reset "
                    "it. Everything else works.";
            return NoticeLevel::Info;
        }

        if (m_cps > kButterflyCPS) {
            *text = "Above 16 is butterfly territory. Reachable by hand, but "
                    "people notice.";
            return NoticeLevel::Warning;
        }
        return NoticeLevel::None;
    }
};
