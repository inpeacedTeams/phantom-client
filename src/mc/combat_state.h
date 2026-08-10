#pragma once
#include <jni.h>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstdio>

#include "minecraft.h"
#include "entity_list.h"
#include "../jni/class_resolver.h"
#include "../jni/jvmti_util.h"

// =================================================================
// CombatState
// =================================================================
// One place that answers the questions every combat module needs,
// computed once per tick.
//
// WHY THIS EXISTS
//
// Every module used to detect "we attacked" with:
//
//     GetAsyncKeyState(VK_LBUTTON) & 0x8000
//
// That is the mouse button, not an attack. It is true when you
// click at the sky, at a block, at a menu, or at nothing. Sprint
// reset fired on air swings. Auto-blockhit released the key for a
// swing that never happened. Backtrack rewound while you were
// mining.
//
// What we actually want is three different facts, and they are not
// the same event:
//
//   SwungThisTick   our arm moved (swingProgress restarted)
//   AttackedThisTick  the swing had a target in front of it
//   HitTakenThisTick  we received damage
//
// EntityPlayerSP.swingProgressInt resets to 0 on every swing and
// counts up afterwards, so the falling edge back to 0 is the swing
// itself. That is the same signal the server sees, which is the
// whole point: modules keyed to it stay in step with the packets
// rather than with the mouse.
// =================================================================

class CombatState {
private:
    using Clock = std::chrono::steady_clock;
    using Ms    = std::chrono::milliseconds;

    // ---- Per-tick facts ----
    inline static bool s_swung        = false;
    inline static bool s_attacked     = false;
    inline static bool s_hitTaken     = false;
    inline static bool s_hasTarget    = false;
    inline static bool s_targetInReach = false;

    inline static double s_targetDist = 99.0;
    inline static int    s_hurtTime   = 0;
    inline static int    s_lastHurt   = 0;

    // ---- History ----
    inline static Clock::time_point s_lastSwing;
    inline static Clock::time_point s_lastAttack;
    inline static Clock::time_point s_lastHit;
    inline static bool s_everSwung  = false;
    inline static bool s_everAttacked = false;
    inline static bool s_everHit   = false;

    inline static int s_ticksSinceSwing  = 9999;
    inline static int s_ticksSinceAttack = 9999;
    inline static int s_ticksSinceHit    = 9999;

    // Rolling CPS, measured from real swings rather than clicks
    inline static std::deque<long long> s_swingGaps;
    inline static float s_cps = 0.0f;

    // ---- Combat engagement ----
    inline static bool s_inCombat = false;
    inline static int  s_combatTicks = 0;

    // ---- Detection state ----
    inline static int  s_lastSwingInt = 0;
    inline static bool s_lastSwingFlag = false;

    // ---- JNI ----
    inline static jfieldID s_fSwingInt  = nullptr;  // swingProgressInt
    inline static jfieldID s_fSwingFlag = nullptr;  // isSwingInProgress
    inline static bool s_resolved = false;
    inline static bool s_usable   = false;

    static long long MsSince(Clock::time_point t) {
        return std::chrono::duration_cast<Ms>(Clock::now() - t).count();
    }

    static void Resolve(JNIEnv* env) {
        if (s_resolved) return;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) return;   // still loading, try again next tick

        // Resolve against the live player's class. Lunar's hierarchy
        // does not always line up with vanilla, and looking these up
        // on a guessed class is how they end up null.
        jclass pc = env->GetObjectClass(player);
        s_fSwingInt = JvmtiUtil::FindField(env, pc,
            { "field_110158_av", "swingProgressInt" });
        s_fSwingFlag = JvmtiUtil::FindField(env, pc,
            { "field_82175_bq", "isSwingInProgress" });
        env->DeleteLocalRef(pc);

        s_usable = (s_fSwingInt != nullptr || s_fSwingFlag != nullptr);
        s_resolved = true;

        printf("[Combat] swingInt=%p swingFlag=%p usable=%d\n",
            (void*)s_fSwingInt, (void*)s_fSwingFlag, (int)s_usable);
    }

public:
    static bool IsUsable() { return s_usable; }

    // -------------------------------------------------------------
    // Called once per tick by ModuleManager, before the modules run
    // and after Backtrack has restored real positions.
    // -------------------------------------------------------------
    static void Update(JNIEnv* env) {
        Resolve(env);

        s_swung = s_attacked = s_hitTaken = false;

        jobject player = Minecraft::GetPlayer(env);
        if (!player) { Reset(); return; }

        if (s_ticksSinceSwing  < 9999) s_ticksSinceSwing++;
        if (s_ticksSinceAttack < 9999) s_ticksSinceAttack++;
        if (s_ticksSinceHit    < 9999) s_ticksSinceHit++;

        // ---- Did our arm move ----
        // swingProgressInt resets to 0 at the start of a swing and
        // counts up. Watching it drop is the same moment the server
        // sees the animation packet.
        bool swung = false;
        if (s_fSwingInt) {
            int now = env->GetIntField(player, s_fSwingInt);
            if (now < s_lastSwingInt || (now == 0 && s_lastSwingInt > 0))
                swung = true;
            // Counter went 0 -> 1: the swing began this tick
            if (s_lastSwingInt == 0 && now == 1) swung = true;
            s_lastSwingInt = now;
        } else if (s_fSwingFlag) {
            // Fallback: the boolean stays true for the whole swing,
            // so only the rising edge counts.
            bool flag = env->GetBooleanField(player, s_fSwingFlag) != 0;
            if (flag && !s_lastSwingFlag) swung = true;
            s_lastSwingFlag = flag;
        }

        if (swung) {
            s_swung = true;

            if (s_everSwung) {
                long long gap = MsSince(s_lastSwing);
                // Ignore duplicates and pauses that say nothing
                if (gap >= 40 && gap <= 2000) {
                    s_swingGaps.push_back(gap);
                    if (s_swingGaps.size() > 10) s_swingGaps.pop_front();

                    long long sum = 0;
                    for (auto v : s_swingGaps) sum += v;
                    long long avg = sum / (long long)s_swingGaps.size();
                    s_cps = avg > 0 ? (float)(1000.0 / (double)avg) : 0.0f;
                }
            }
            s_lastSwing = Clock::now();
            s_everSwung = true;
            s_ticksSinceSwing = 0;
        }

        // ---- Did we take damage ----
        s_hurtTime = Minecraft::GetHurtTime(env, player);
        if (s_hurtTime > 0 && s_lastHurt == 0) {
            s_hitTaken = true;
            s_lastHit = Clock::now();
            s_everHit = true;
            s_ticksSinceHit = 0;
        }
        s_lastHurt = s_hurtTime;

        // ---- Was anything in front of that swing ----
        s_hasTarget = false;
        s_targetInReach = false;
        s_targetDist = 99.0;

        if (EntityList::Init(env)) {
            auto ents = EntityList::GetPlayers(env, 6.0f);
            for (auto& e : ents) {
                if (e.distanceToPlayer < s_targetDist)
                    s_targetDist = e.distanceToPlayer;
            }
            s_hasTarget = !ents.empty();
            s_targetInReach = (s_targetDist <= 3.5);
        }

        // A swing only counts as an attack if something was there to
        // hit. This is the distinction the old mouse-button check
        // could not make.
        if (s_swung && s_targetInReach) {
            s_attacked = true;
            s_lastAttack = Clock::now();
            s_everAttacked = true;
            s_ticksSinceAttack = 0;
        }

        // ---- Engagement ----
        // Being in a fight is the union of hitting and being hit,
        // with a tail so it does not flicker between exchanges.
        bool active = (s_ticksSinceAttack < 40) || (s_ticksSinceHit < 40);
        if (active) {
            s_inCombat = true;
            s_combatTicks++;
        } else {
            s_inCombat = false;
            s_combatTicks = 0;
        }
    }

    static void Reset() {
        s_swung = s_attacked = s_hitTaken = false;
        s_hasTarget = s_targetInReach = false;
        s_inCombat = false;
        s_combatTicks = 0;
        s_lastSwingInt = 0;
        s_lastSwingFlag = false;
        s_swingGaps.clear();
        s_cps = 0.0f;
    }

    // ---- This tick ----
    static bool SwungThisTick()    { return s_swung; }
    static bool AttackedThisTick() { return s_attacked; }
    static bool HitTakenThisTick() { return s_hitTaken; }

    // ---- Recency, in ticks ----
    static int TicksSinceSwing()  { return s_ticksSinceSwing; }
    static int TicksSinceAttack() { return s_ticksSinceAttack; }
    static int TicksSinceHit()    { return s_ticksSinceHit; }

    // ---- Recency, in milliseconds ----
    static long long MsSinceSwing()  { return s_everSwung  ? MsSince(s_lastSwing)  : 999999; }
    static long long MsSinceAttack() { return s_everAttacked ? MsSince(s_lastAttack) : 999999; }
    static long long MsSinceHit()    { return s_everHit    ? MsSince(s_lastHit)    : 999999; }

    // ---- Situation ----
    static bool   HasTarget()     { return s_hasTarget; }
    static bool   TargetInReach() { return s_targetInReach; }
    static double TargetDist()    { return s_targetDist; }
    static int    HurtTime()      { return s_hurtTime; }
    static bool   InCombat()      { return s_inCombat; }
    static int    CombatTicks()   { return s_combatTicks; }

    // Real measured click rate, from swings rather than mouse polls
    static float CPS() { return s_cps; }
};
