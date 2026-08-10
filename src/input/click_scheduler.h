#pragma once
#include <Windows.h>
// WIN32_LEAN_AND_MEAN keeps mmsystem.h out of Windows.h, which is
// where timeBeginPeriod normally lives. Pull in the timer API on
// its own so the build does not fail on an undeclared identifier.
#include <timeapi.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <deque>
#include <random>
#include <cmath>

#pragma comment(lib, "winmm.lib")

// =================================================================
// ClickScheduler
// =================================================================
// TWO THINGS WERE WRONG WITH THE OLD ONE, AND BOTH WERE FATAL.
//
// 1. IT CLICKED WITH SendInput, WHICH CANNOT WORK HERE.
//
//    The autoclicker only runs while you hold left mouse. So the
//    physical button is already down, and Windows tracks one
//    global button state. Injecting MOUSEEVENTF_LEFTDOWN for a
//    button the OS already considers down is not a state change,
//    so nothing is delivered. The following LEFTUP is a change, so
//    that one goes through.
//
//    Net effect: the game receives a stream of releases and no
//    presses. Zero clicks, and the mouse state left inconsistent
//    with the hardware. That is the bug.
//
//    We now queue clicks into KeyBinding.pressTime, which is the
//    counter Minecraft.runTick() drains with
//        while (keyBindAttack.isPressed()) clickMouse();
//    It is literally the same field a real click increments, so
//    the swing, the attack packet and the timing are identical to
//    a human click. No OS input, no foreground games, no conflict
//    with the hardware button.
//
// 2. IT CALLED BACK INTO THE MODULE FROM THE TIMER THREAD.
//
//    The delay came from a std::function owned by ClickAssist,
//    invoked on the click thread while holding a lock, while the
//    render thread read the same module state to draw its panel.
//    Three threads, one object, and a module pointer that dies on
//    eject. That is a crash waiting for a slow frame.
//
//    All timing lives here now. The module publishes a plain POD
//    profile under a short lock and never gets called back.
//
// WHAT MAKES A CLICK STREAM LOOK HUMAN
//   * standard deviation of the gaps. Too low means machine.
//   * stray long gaps. Hands produce them, loops do not.
//   * butterfly makes tight PAIRS, not an even stream. A flat 50ms
//     cadence at 20 CPS is not reachable by a hand.
//   * drift. Real hands fatigue and the rate sags.
//   * nothing under about 20ms, which is physically impossible.
//
// The 1ms timer thread exists because the 20 TPS module loop
// cannot express a 27ms gap: every request landed on the next 50ms
// boundary and produced a dead flat stream, the exact signature we
// are trying not to have.
// =================================================================

struct ClickProfile {
    int   pattern        = 1;    // 0 normal, 1 butterfly, 2 drag, 3 jitter
    float minCPS         = 15.f;
    float maxCPS         = 20.f;

    // Butterfly
    int   pairGapMin     = 22;
    int   pairGapMax     = 38;
    int   restGapMin     = 62;
    int   restGapMax     = 98;
    float pairSkipChance = 6.f;

    // Drag
    int   burstLenMin    = 3;
    int   burstLenMax    = 7;
    int   burstGapMin    = 90;
    int   burstGapMax    = 170;

    // Humanisation
    bool  jitter         = true;
    float jitterAmount   = 26.f;
    bool  fatigue        = true;
    float fatigueRate    = 12.f;
    int   fatigueAfterMs = 2600;
    bool  outliers       = true;
    float outlierChance  = 4.f;
    int   outlierAddMin  = 40;
    int   outlierAddMax  = 120;

    // Safety
    int   floorMs        = 24;
    bool  entropyGuard   = true;
    float minStdDev      = 9.f;
    bool  breaks         = true;
    float breakChance    = 8.f;
    int   breakLenMin    = 2;
    int   breakLenMax    = 5;
};

class ClickScheduler {
private:
    using Clock = std::chrono::steady_clock;

    inline static std::thread       s_thread;
    inline static std::atomic<bool> s_running{ false };
    inline static std::atomic<bool> s_active{ false };
    inline static std::atomic<bool> s_rightButton{ false };

    inline static HWND s_window = nullptr;

    // ---- Profile, published by the module ----
    inline static ClickProfile s_profile;
    inline static std::mutex   s_profileMutex;

    // ---- Pending clicks, drained by the client thread ----
    inline static std::atomic<int> s_pendingLeft{ 0 };
    inline static std::atomic<int> s_pendingRight{ 0 };

    // A backlog this deep means the game is stalled or we are in a
    // menu. Releasing it all at once would be a burst no hand makes,
    // so extra clicks are dropped instead of stored.
    static constexpr int kMaxPending = 3;

    // ---- Shared rate floor, honoured by every click source ----
    inline static std::mutex   s_clickMutex;
    inline static Clock::time_point s_lastClick;
    inline static std::atomic<int>  s_floorMs{ 24 };

    // ---- Pattern state, only ever touched on the timer thread ----
    inline static std::mt19937 s_rng{ std::random_device{}() };
    inline static bool s_inPair    = false;
    inline static int  s_burstLeft = 0;
    inline static int  s_breakLeft = 0;
    inline static Clock::time_point s_holdStart;

    // ---- Stats, read by the UI ----
    inline static std::mutex s_statsMutex;
    inline static std::deque<long long> s_history;
    inline static std::atomic<long long> s_lastGap{ 0 };
    inline static std::atomic<long long> s_emitted{ 0 };
    inline static std::atomic<long long> s_rejected{ 0 };
    inline static std::atomic<long long> s_dropped{ 0 };

    static int Rand(int lo, int hi) {
        if (lo >= hi) return lo;
        return std::uniform_int_distribution<int>(lo, hi)(s_rng);
    }
    static bool Roll(float pct) {
        return std::uniform_real_distribution<float>(0.f, 100.f)(s_rng) < pct;
    }

    static float StdDev() {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        if (s_history.size() < 6) return 999.f;
        double mean = 0.0;
        for (auto v : s_history) mean += (double)v;
        mean /= (double)s_history.size();
        double var = 0.0;
        for (auto v : s_history) { double d = (double)v - mean; var += d * d; }
        var /= (double)s_history.size();
        return (float)std::sqrt(var);
    }

    static float MeanGap() {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        if (s_history.empty()) return 0.f;
        double sum = 0.0;
        for (auto v : s_history) sum += (double)v;
        return (float)(sum / (double)s_history.size());
    }

    static void Record(long long gap) {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        s_history.push_back(gap);
        if (s_history.size() > 30) s_history.pop_front();
    }

    // Hands slow down. A rate that holds exactly steady for a whole
    // fight is its own signature.
    static float FatigueFactor(const ClickProfile& p) {
        if (!p.fatigue) return 1.0f;
        auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - s_holdStart).count();
        if (held < p.fatigueAfterMs) return 1.0f;
        float over = (float)(held - p.fatigueAfterMs) / 4000.0f;
        if (over > 1.0f) over = 1.0f;
        float f = 1.0f - (p.fatigueRate / 100.0f) * over;
        return f < 0.3f ? 0.3f : f;
    }

    static long long ApplyNoise(const ClickProfile& p, long long delay) {
        if (p.jitter && p.jitterAmount > 0.f) {
            float range = (float)delay * (p.jitterAmount / 100.0f);
            delay += (long long)std::uniform_real_distribution<float>(
                -range, range)(s_rng);
        }
        if (p.outliers && Roll(p.outlierChance))
            delay += Rand(p.outlierAddMin, p.outlierAddMax);

        // If the recent stream has flattened out, widen it back up
        if (p.entropyGuard && StdDev() < p.minStdDev) {
            delay += (long long)std::uniform_real_distribution<float>(
                -p.minStdDev * 1.6f, p.minStdDev * 1.6f)(s_rng);
        }

        long long floorMs = (long long)p.floorMs;
        if (delay < floorMs) delay = floorMs;
        return delay;
    }

    static long long NextDelay() {
        ClickProfile p;
        {
            std::lock_guard<std::mutex> lock(s_profileMutex);
            p = s_profile;                 // copy, then release
        }

        float fatigue = FatigueFactor(p);

        // Occasional lapses. Perfect cadence forever is a pattern.
        if (p.breaks) {
            if (s_breakLeft > 0) {
                s_breakLeft--;
                float c = p.minCPS * 0.55f;
                if (c < 1.f) c = 1.f;
                return ApplyNoise(p, (long long)(1000.0f / c));
            }
            if (Roll(p.breakChance * 0.08f))
                s_breakLeft = Rand(p.breakLenMin, p.breakLenMax);
        }

        long long delay;
        switch (p.pattern) {
            case 1: {   // Butterfly: tight pair, then a rest
                if (s_inPair) {
                    delay = Rand(p.pairGapMin, p.pairGapMax);
                    s_inPair = false;
                } else {
                    delay = Rand(p.restGapMin, p.restGapMax);
                    s_inPair = !Roll(p.pairSkipChance);
                }
                delay = (long long)((float)delay / fatigue);
                break;
            }
            case 2: {   // Drag: dense burst, longer recovery
                if (s_burstLeft > 0) {
                    s_burstLeft--;
                    float c = p.maxCPS < 1.f ? 1.f : p.maxCPS;
                    delay = (long long)(1000.0f / c);
                } else {
                    s_burstLeft = Rand(p.burstLenMin, p.burstLenMax);
                    delay = Rand(p.burstGapMin, p.burstGapMax);
                }
                delay = (long long)((float)delay / fatigue);
                break;
            }
            case 3: {   // Jitter: wide single stream
                float mean = (p.minCPS + p.maxCPS) * 0.5f * fatigue;
                float sd   = (p.maxCPS - p.minCPS) * 0.42f;
                if (sd < 0.1f) sd = 0.1f;
                float cps = std::normal_distribution<float>(mean, sd)(s_rng);
                if (cps < 1.f) cps = 1.f;
                delay = (long long)(1000.0f / cps);
                break;
            }
            default: {  // Normal
                float mean = (p.minCPS + p.maxCPS) * 0.5f * fatigue;
                float sd   = (p.maxCPS - p.minCPS) * 0.25f;
                if (sd < 0.1f) sd = 0.1f;
                float cps = std::normal_distribution<float>(mean, sd)(s_rng);
                float lo = p.minCPS * 0.6f;
                if (cps < lo) cps = lo;
                if (cps > p.maxCPS) cps = p.maxCPS;
                if (cps < 1.f) cps = 1.f;
                delay = (long long)(1000.0f / cps);
                break;
            }
        }

        return ApplyNoise(p, delay);
    }

    // Sleep for most of the wait, then spin the remainder. Sleep on
    // its own overshoots by several ms even at 1ms resolution, which
    // would flatten the interval distribution all over again.
    //
    // Returns false if we were told to stop mid-wait, so a toggle
    // does not have to wait out a 200ms break gap.
    static bool PreciseWait(long long ms) {
        if (ms <= 0) return true;

        auto target = Clock::now() + std::chrono::milliseconds(ms);

        while (Clock::now() < target) {
            if (!s_running.load(std::memory_order_relaxed)) return false;
            if (!s_active.load(std::memory_order_relaxed))  return false;

            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                target - Clock::now()).count();
            if (left > 4) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            else          YieldProcessor();
        }
        return true;
    }

    static void Loop() {
        timeBeginPeriod(1);

        while (s_running.load(std::memory_order_relaxed)) {
            if (!s_active.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }

            // Never click into another application
            if (s_window && GetForegroundWindow() != s_window) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            long long delay = NextDelay();
            if (delay <= 0) delay = 50;

            if (!PreciseWait(delay)) continue;

            if (Emit(s_rightButton.load(std::memory_order_relaxed))) {
                s_lastGap.store(delay, std::memory_order_relaxed);
                Record(delay);
            }
        }

        timeEndPeriod(1);
    }

    // The single place a click enters the queue. Returns false when
    // the shared floor or the backlog cap refused it.
    static bool Emit(bool right) {
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(s_clickMutex);
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s_lastClick).count();
            if (since < s_floorMs.load(std::memory_order_relaxed)) {
                s_rejected.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            s_lastClick = now;
        }

        std::atomic<int>& slot = right ? s_pendingRight : s_pendingLeft;
        int cur = slot.load(std::memory_order_relaxed);
        if (cur >= kMaxPending) {
            s_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        slot.fetch_add(1, std::memory_order_relaxed);
        s_emitted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

public:
    static void Start() {
        if (s_running.exchange(true)) return;
        s_lastClick = Clock::now();
        s_holdStart = Clock::now();
        s_thread = std::thread(Loop);
    }

    static void Stop() {
        if (!s_running.exchange(false)) return;
        s_active.store(false);
        if (s_thread.joinable()) s_thread.join();
        s_pendingLeft.store(0);
        s_pendingRight.store(0);
    }

    static void SetWindow(HWND hwnd) { s_window = hwnd; }

    // Publishes the shape of the click stream. Cheap enough to call
    // every tick; the timer thread copies it and never reaches back
    // into the module.
    static void SetProfile(const ClickProfile& p) {
        {
            std::lock_guard<std::mutex> lock(s_profileMutex);
            s_profile = p;
        }
        s_floorMs.store(p.floorMs < 1 ? 1 : p.floorMs);
    }

    static ClickProfile GetProfile() {
        std::lock_guard<std::mutex> lock(s_profileMutex);
        return s_profile;
    }

    // -------------------------------------------------------------
    // Drained on the client thread, which is the only thread allowed
    // to touch JNI. Returns how many clicks to hand the game.
    // -------------------------------------------------------------
    static int DrainLeft()  { return s_pendingLeft.exchange(0); }
    static int DrainRight() { return s_pendingRight.exchange(0); }

    static void ClearPending() {
        s_pendingLeft.store(0);
        s_pendingRight.store(0);
    }

    // One-off click from a module that is not the autoclicker, such
    // as Hit Select. Goes through the same floor, so it can never
    // stack with the autoclicker into an impossible interval.
    static bool RequestClick(bool right = false) {
        if (s_window && GetForegroundWindow() != s_window) return false;
        return Emit(right);
    }

    static void SetActive(bool on) {
        bool was = s_active.exchange(on);
        if (on && !was) {
            // A new hold starts a new fatigue curve and a clean
            // history, otherwise the stats describe the last fight.
            s_holdStart = Clock::now();
            s_inPair = false;
            s_burstLeft = 0;
            s_breakLeft = 0;
            std::lock_guard<std::mutex> lock(s_statsMutex);
            s_history.clear();
        }
        if (!on) ClearPending();
    }

    static bool IsActive()             { return s_active.load(); }
    static bool IsRunning()            { return s_running.load(); }
    static void SetRightButton(bool r) { s_rightButton.store(r); }

    // ---- Readouts ----
    static long long LastGap()  { return s_lastGap.load(); }
    static long long Emitted()  { return s_emitted.load(); }
    static long long Rejected() { return s_rejected.load(); }
    static long long Dropped()  { return s_dropped.load(); }
    static int  GetFloor()      { return s_floorMs.load(); }
    static float LiveStdDev()   { return StdDev(); }

    static float LiveCPS() {
        float mean = MeanGap();
        return mean > 1.f ? 1000.0f / mean : 0.f;
    }
};
