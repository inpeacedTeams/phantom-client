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
// ClickScheduler — the autoclicker engine
// =================================================================
// WHY CLICKS GO THROUGH THE GAME AND NOT THROUGH WINDOWS
//
// The obvious implementation, SendInput, cannot work here. The
// clicker only runs while you HOLD the physical button, so Windows
// already considers it down. Injecting another LEFTDOWN is not a
// state change and is silently discarded, while the matching
// LEFTUP is a change and goes through. The game receives a stream
// of releases and not one press.
//
// Instead a counter is handed to Minecraft:
//
//     Minecraft.runTick():
//         while (gameSettings.keyBindAttack.isPressed()) clickMouse();
//
//     KeyBinding.isPressed():
//         if (pressTime == 0) return false;
//         --pressTime; return true;
//
// pressTime is the same field a real click increments, so the
// swing, the attack and the packet are identical to a hand.
//
// -----------------------------------------------------------------
// WHY THE ENGINE OWNS THE BUTTON
// -----------------------------------------------------------------
// The module used to poll the mouse on the 20 TPS client tick and
// tell the engine to start or stop. That put up to 50ms of slack on
// both ends: a late first click, and worse, clicks still landing
// after you let go. Both are exactly what you notice.
//
// The timer thread polls the button itself at 1ms. Press to first
// click is about a millisecond, release is immediate, and the
// pending queue is dropped on the same edge so nothing trails.
//
// -----------------------------------------------------------------
// WHAT MAKES A CLICK STREAM LOOK HUMAN
// -----------------------------------------------------------------
// Uniform random delays are not human. They are white noise: every
// interval independent of the last, a flat histogram, and a spread
// that never changes. Real hands do three things a naive clicker
// does not.
//
//   1. They are NORMALLY distributed, not uniform. Most clicks land
//      near your natural rate with a thinning tail either side, so
//      intervals come from a Gaussian (Box-Muller).
//
//   2. They DRIFT, and consecutive intervals are correlated. You
//      speed up for a second, then ease off. That is modelled with
//      an Ornstein-Uhlenbeck process: a slow random walk pulled
//      back toward your mean rate. Without it the rate is
//      statistically stationary, which is its own fingerprint.
//
//   3. They FUMBLE. Occasional long gaps where attention slips, and
//      occasional bursts where it spikes.
//
// The floor exists because nothing under about 20ms is physically
// reachable, and one sub-20ms interval is worth more to an
// anticheat than a thousand ordinary ones.
// =================================================================

struct ClickerConfig {
    // ---- Rate ----
    float cps      = 12.0f;    // what the player asked for
    float variance = 18.0f;    // percent spread around it

    // ---- Humanisation ----
    bool  drift        = true;   // slow correlated wander
    float driftAmount  = 12.0f;  // percent
    bool  fumbles      = true;   // rare long gaps
    float fumbleChance = 3.0f;
    bool  bursts       = true;   // rare short gaps
    float burstChance  = 4.0f;

    // ---- Safety ----
    int   floorMs = 22;          // never faster than this

    // ---- Behaviour ----
    bool  rightButton = false;   // drive use-item instead of attack
};

class ClickScheduler {
private:
    using Clock = std::chrono::steady_clock;

    inline static std::thread       s_thread;
    inline static std::atomic<bool> s_running{ false };

    // Armed: the module is on and the client says clicking is legal
    // (in a world, not in a menu). Holding: the button is physically
    // down. Both must be true.
    inline static std::atomic<bool> s_armed{ false };
    inline static std::atomic<bool> s_holding{ false };

    inline static HWND s_window = nullptr;

    // ---- Config, published by the module ----
    inline static ClickerConfig s_cfg;
    inline static std::mutex    s_cfgMutex;

    // ---- Pending clicks, drained by the client thread ----
    inline static std::atomic<int> s_pendingLeft{ 0 };
    inline static std::atomic<int> s_pendingRight{ 0 };

    // A backlog this deep means the game is stalled or a screen is
    // open. Releasing it in one tick would be a burst no hand makes,
    // so extras are dropped rather than stored.
    static constexpr int kMaxPending = 3;

    // ---- Shared rate floor, honoured by every click source ----
    inline static std::mutex        s_clickMutex;
    inline static Clock::time_point s_lastClick;
    inline static std::atomic<int>  s_floorMs{ 22 };

    // ---- Generator state, timer thread only ----
    inline static std::mt19937 s_rng{ std::random_device{}() };
    inline static double s_driftState = 0.0;    // the OU process
    inline static Clock::time_point s_holdStart;

    // ---- Stats ----
    inline static std::mutex s_statsMutex;
    inline static std::deque<long long> s_history;
    inline static std::atomic<long long> s_lastGap{ 0 };
    inline static std::atomic<long long> s_emitted{ 0 };
    inline static std::atomic<long long> s_dropped{ 0 };
    inline static std::atomic<long long> s_floored{ 0 };

    // Box-Muller. std::normal_distribution would do, but this keeps
    // the generator explicit and identical across compilers.
    static double Gauss(double mean, double sigma) {
        static thread_local bool have = false;
        static thread_local double spare = 0.0;

        if (have) { have = false; return mean + sigma * spare; }

        std::uniform_real_distribution<double> u(1e-9, 1.0);
        double u1 = u(s_rng), u2 = u(s_rng);
        double mag = std::sqrt(-2.0 * std::log(u1));
        double z0 = mag * std::cos(6.283185307179586 * u2);
        spare     = mag * std::sin(6.283185307179586 * u2);
        have = true;
        return mean + sigma * z0;
    }

    static bool Roll(float pct) {
        return std::uniform_real_distribution<float>(0.f, 100.f)(s_rng) < pct;
    }

    static void Record(long long gap) {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        s_history.push_back(gap);
        if (s_history.size() > 40) s_history.pop_front();
    }

    static float MeanGap() {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        if (s_history.empty()) return 0.f;
        double sum = 0.0;
        for (auto v : s_history) sum += (double)v;
        return (float)(sum / (double)s_history.size());
    }

    static float StdDev() {
        std::lock_guard<std::mutex> lock(s_statsMutex);
        if (s_history.size() < 6) return 0.f;
        double mean = 0.0;
        for (auto v : s_history) mean += (double)v;
        mean /= (double)s_history.size();
        double var = 0.0;
        for (auto v : s_history) { double d = (double)v - mean; var += d * d; }
        return (float)std::sqrt(var / (double)s_history.size());
    }

    // -------------------------------------------------------------
    // Ornstein-Uhlenbeck step.
    //
    //   dx = -theta * x * dt + sigma * sqrt(dt) * N(0,1)
    //
    // theta pulls the value back toward zero, so the rate wanders
    // but never runs away. sigma sets how far it strays. The result
    // is temporally CORRELATED noise, which is the part plain
    // randomisation cannot fake.
    // -------------------------------------------------------------
    static double StepDrift(double dtSeconds, double sigma) {
        const double theta = 0.75;
        s_driftState += -theta * s_driftState * dtSeconds
                      + sigma * std::sqrt(dtSeconds) * Gauss(0.0, 1.0);

        // Hard bound: a runaway walk would park the rate at an
        // extreme for seconds at a time.
        if (s_driftState >  2.5) s_driftState =  2.5;
        if (s_driftState < -2.5) s_driftState = -2.5;
        return s_driftState;
    }

    // The next interval, in milliseconds.
    static long long NextDelay() {
        ClickerConfig c;
        {
            std::lock_guard<std::mutex> lock(s_cfgMutex);
            c = s_cfg;
        }

        float target = c.cps;
        if (target < 1.0f)  target = 1.0f;
        if (target > 25.0f) target = 25.0f;

        double base = 1000.0 / (double)target;

        // ---- Spread ----
        // Variance is expressed against the interval rather than the
        // rate: a hand's timing error is roughly proportional to the
        // gap it is trying to hit.
        double sigma = base * (double)(c.variance * 0.01f);
        double delay = Gauss(base, sigma);

        // ---- Drift ----
        if (c.drift) {
            double d = StepDrift(base / 1000.0, 0.9);
            delay *= 1.0 + d * (double)(c.driftAmount * 0.01f) * 0.4;
        }

        // ---- Fumbles and bursts ----
        if (c.fumbles && Roll(c.fumbleChance)) {
            std::uniform_real_distribution<double> f(1.4, 2.6);
            delay *= f(s_rng);
        } else if (c.bursts && Roll(c.burstChance)) {
            std::uniform_real_distribution<double> f(0.62, 0.85);
            delay *= f(s_rng);
        }

        // Gaussian tails can go negative. Never let the mean collapse.
        double minSane = base * 0.35;
        if (delay < minSane) delay = minSane;

        long long ms = (long long)(delay + 0.5);
        long long floorMs = (long long)c.floorMs;
        if (ms < floorMs) {
            ms = floorMs;
            s_floored.fetch_add(1, std::memory_order_relaxed);
        }
        return ms;
    }

    static bool ShouldRun() {
        if (!s_armed.load(std::memory_order_relaxed)) return false;
        if (!s_holding.load(std::memory_order_relaxed)) return false;
        if (s_window && GetForegroundWindow() != s_window) return false;
        return true;
    }

    // The physical button. Read here rather than on the client tick
    // so press and release are acted on within a millisecond.
    static void PollButton() {
        bool right;
        {
            std::lock_guard<std::mutex> lock(s_cfgMutex);
            right = s_cfg.rightButton;
        }
        int vk = right ? VK_RBUTTON : VK_LBUTTON;
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;

        bool was = s_holding.exchange(down, std::memory_order_relaxed);

        if (down && !was) {
            // Fresh hold: new drift curve, clean statistics.
            s_holdStart  = Clock::now();
            s_driftState = 0.0;
            std::lock_guard<std::mutex> lock(s_statsMutex);
            s_history.clear();
        } else if (!down && was) {
            // Released. Anything already queued must go, or clicks
            // land after the player let go.
            ClearPending();
        }
    }

    // Sleep for most of the wait, then spin the last couple of
    // milliseconds. Sleep alone overshoots even at 1ms resolution,
    // and that overshoot would quantise the distribution we just
    // went to the trouble of shaping.
    //
    // Returns false the moment the button comes up, so releasing
    // never has to wait out a long gap.
    static bool PreciseWait(long long ms) {
        if (ms <= 0) return true;
        auto target = Clock::now() + std::chrono::milliseconds(ms);

        while (Clock::now() < target) {
            if (!s_running.load(std::memory_order_relaxed)) return false;

            PollButton();
            if (!ShouldRun()) return false;

            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                target - Clock::now()).count();
            if (left > 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else          YieldProcessor();
        }
        return true;
    }

    static void Loop() {
        timeBeginPeriod(1);

        while (s_running.load(std::memory_order_relaxed)) {
            PollButton();

            if (!ShouldRun()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            long long delay = NextDelay();
            if (!PreciseWait(delay)) continue;
            if (!ShouldRun()) continue;

            bool right;
            {
                std::lock_guard<std::mutex> lock(s_cfgMutex);
                right = s_cfg.rightButton;
            }

            if (Emit(right)) {
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
                s_dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            s_lastClick = now;
        }

        std::atomic<int>& slot = right ? s_pendingRight : s_pendingLeft;
        if (slot.load(std::memory_order_relaxed) >= kMaxPending) {
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
        s_armed.store(false);
        if (s_thread.joinable()) s_thread.join();
        ClearPending();
    }

    static void SetWindow(HWND hwnd) { s_window = hwnd; }

    // Published by the module every tick. Cheap, and the timer
    // thread only ever copies it: no callback into module memory.
    static void SetConfig(const ClickerConfig& c) {
        {
            std::lock_guard<std::mutex> lock(s_cfgMutex);
            s_cfg = c;
        }
        s_floorMs.store(c.floorMs < 1 ? 1 : c.floorMs);
    }

    static ClickerConfig GetConfig() {
        std::lock_guard<std::mutex> lock(s_cfgMutex);
        return s_cfg;
    }

    // The module is on and the world allows clicking. Not the same
    // as the button being down, which the engine reads itself.
    static void SetArmed(bool on) {
        bool was = s_armed.exchange(on);
        if (was && !on) ClearPending();
    }

    static bool IsArmed()    { return s_armed.load(); }
    static bool IsHolding()  { return s_holding.load(); }
    static bool IsRunning()  { return s_running.load(); }
    static bool IsClicking() { return s_armed.load() && s_holding.load(); }

    // -------------------------------------------------------------
    // Drained on the client thread, the only one allowed to touch
    // JNI. Returns how many clicks to hand the game.
    // -------------------------------------------------------------
    static int DrainLeft()  { return s_pendingLeft.exchange(0); }
    static int DrainRight() { return s_pendingRight.exchange(0); }

    static void ClearPending() {
        s_pendingLeft.store(0);
        s_pendingRight.store(0);
    }

    // One-off click from a module that is not the autoclicker, such
    // as Hit Select. Shares the floor, so the two can never stack
    // into an interval no hand could produce.
    static bool RequestClick(bool right = false) {
        if (s_window && GetForegroundWindow() != s_window) return false;
        return Emit(right);
    }

    // ---- Readouts ----
    static long long LastGap() { return s_lastGap.load(); }
    static long long Emitted() { return s_emitted.load(); }
    static long long Dropped() { return s_dropped.load(); }
    static long long Floored() { return s_floored.load(); }
    static int   GetFloor()    { return s_floorMs.load(); }
    static float LiveStdDev()  { return StdDev(); }

    static float LiveCPS() {
        float mean = MeanGap();
        return mean > 1.f ? 1000.0f / mean : 0.f;
    }
};
