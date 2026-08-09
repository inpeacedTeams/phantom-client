#pragma once
#include <Windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>

#pragma comment(lib, "winmm.lib")

// =================================================================
// ClickScheduler
// =================================================================
// WHY THIS EXISTS
//
// Click timing was driven from the 20 TPS module loop, so the
// smallest gap it could produce was one tick: 50ms. Every
// "click after 27ms" request landed on the next 50ms boundary.
//
// The result was the exact opposite of the intent: a perfectly
// flat 20 CPS stream with a standard deviation near zero, which
// is the clearest possible machine signature. Butterfly pairs
// (22-38ms apart) could not exist at all.
//
// This runs clicks on their own thread at 1ms resolution:
//   - timeBeginPeriod(1) raises the system timer resolution,
//     otherwise Sleep() rounds up to ~15.6ms
//   - a spin-wait covers the last 2ms, where Sleep is unreliable
//   - the module supplies the next delay through a callback, so
//     all the pattern logic stays where it belongs
//
// The click itself goes out via SendInput, which the game reads
// as a normal hardware mouse event.
// =================================================================

class ClickScheduler {
public:
    // Returns the delay in ms before the next click.
    // Return 0 to stop clicking until Resume() is called again.
    using DelayProvider = std::function<long long()>;

private:
    inline static std::thread        s_thread;
    inline static std::atomic<bool>  s_running{ false };
    inline static std::atomic<bool>  s_active{ false };   // emitting or idle
    inline static std::atomic<bool>  s_rightButton{ false };

    inline static DelayProvider s_provider;
    inline static std::mutex    s_providerMutex;

    inline static HWND s_window = nullptr;
    inline static std::atomic<long long> s_lastGap{ 0 };

    static void SendClick(bool right) {
        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE;
        in[1].type = INPUT_MOUSE;
        if (right) {
            in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        } else {
            in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        }
        SendInput(2, in, sizeof(INPUT));
    }

    // Sleep for most of it, then spin the remainder. Sleep alone
    // overshoots by several ms even at 1ms timer resolution, which
    // would flatten the distribution all over again.
    static void PreciseWait(long long ms) {
        if (ms <= 0) return;

        auto target = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(ms);

        if (ms > 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms - 2));
        }
        while (std::chrono::steady_clock::now() < target) {
            if (!s_running.load(std::memory_order_relaxed)) return;
            YieldProcessor();
        }
    }

    static void Loop() {
        timeBeginPeriod(1);

        while (s_running.load(std::memory_order_relaxed)) {
            if (!s_active.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }

            // Never type into another application
            if (s_window && GetForegroundWindow() != s_window) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            long long delay = 0;
            {
                std::lock_guard<std::mutex> lock(s_providerMutex);
                if (s_provider) delay = s_provider();
            }

            if (delay <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }

            PreciseWait(delay);
            if (!s_running.load(std::memory_order_relaxed)) break;
            if (!s_active.load(std::memory_order_relaxed))  continue;

            SendClick(s_rightButton.load(std::memory_order_relaxed));
            s_lastGap.store(delay, std::memory_order_relaxed);
        }

        timeEndPeriod(1);
    }

public:
    static void Start() {
        if (s_running.exchange(true)) return;
        s_thread = std::thread(Loop);
    }

    static void Stop() {
        if (!s_running.exchange(false)) return;
        s_active.store(false);
        if (s_thread.joinable()) s_thread.join();
        std::lock_guard<std::mutex> lock(s_providerMutex);
        s_provider = nullptr;
    }

    static void SetWindow(HWND hwnd) { s_window = hwnd; }

    static void SetProvider(DelayProvider fn) {
        std::lock_guard<std::mutex> lock(s_providerMutex);
        s_provider = std::move(fn);
    }

    static void ClearProvider() {
        s_active.store(false);
        std::lock_guard<std::mutex> lock(s_providerMutex);
        s_provider = nullptr;
    }

    static void SetActive(bool on)   { s_active.store(on); }
    static bool IsActive()           { return s_active.load(); }
    static void SetRightButton(bool r) { s_rightButton.store(r); }
    static long long LastGap()       { return s_lastGap.load(); }
};
