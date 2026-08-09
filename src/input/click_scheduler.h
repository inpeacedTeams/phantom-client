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
#include <functional>

#pragma comment(lib, "winmm.lib")

// =================================================================
// ClickScheduler
// =================================================================
// WHY THIS EXISTS
//
// Click timing used to be driven from the 20 TPS module loop, so
// the smallest gap it could produce was one tick: 50ms. Every
// "click after 27ms" request landed on the next 50ms boundary.
//
// The result was the opposite of the intent: a perfectly flat
// 20 CPS stream with a standard deviation near zero, which is the
// clearest possible machine signature. Butterfly pairs, which sit
// 22-38ms apart, could not exist at all.
//
// Clicks now run on their own thread at 1ms resolution:
//   - timeBeginPeriod(1) raises the system timer resolution;
//     without it Sleep() rounds up to roughly 15.6ms
//   - a short spin covers the last 2ms, where Sleep overshoots
//   - the module supplies each delay through a callback, so all
//     the pattern logic stays where it belongs
//
// SHARED FLOOR
// More than one module wants to click: the autoclicker on its own
// rhythm, hit-select on a specific tick. Left uncoordinated they
// stack and produce sub-20ms gaps, which no hand can make and every
// anticheat looks for. Every click in the client goes through here
// and is refused if it lands too soon after the last one.
// =================================================================

class ClickScheduler {
public:
    // Returns the delay in ms before the next click.
    using DelayProvider = std::function<long long()>;

private:
    inline static std::thread       s_thread;
    inline static std::atomic<bool> s_running{ false };
    inline static std::atomic<bool> s_active{ false };
    inline static std::atomic<bool> s_rightButton{ false };

    inline static DelayProvider s_provider;
    inline static std::mutex    s_providerMutex;

    inline static HWND s_window = nullptr;
    inline static std::atomic<long long> s_lastGap{ 0 };

    // Shared across every click source in the client
    inline static std::mutex s_clickMutex;
    inline static std::chrono::steady_clock::time_point s_lastClick;
    inline static std::atomic<int> s_floorMs{ 22 };
    inline static std::atomic<long long> s_rejected{ 0 };

    static void RawClick(bool right) {
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

    // Sleep for most of the wait, then spin the remainder. Sleep on
    // its own overshoots by several ms even at 1ms resolution, which
    // would flatten the interval distribution all over again.
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

            if (Emit(s_rightButton.load(std::memory_order_relaxed)))
                s_lastGap.store(delay, std::memory_order_relaxed);
        }

        timeEndPeriod(1);
    }

    // The single place a click leaves the client. Returns false when
    // the floor rejected it.
    static bool Emit(bool right) {
        auto now = std::chrono::steady_clock::now();
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
        RawClick(right);
        return true;
    }

public:
    static void Start() {
        if (s_running.exchange(true)) return;
        s_lastClick = std::chrono::steady_clock::now();
        s_thread = std::thread(Loop);
    }

    static void Stop() {
        if (!s_running.exchange(false)) return;
        s_active.store(false);
        if (s_thread.joinable()) s_thread.join();
        // Only clear the callback once the thread is joined: it
        // captures a module pointer that is about to be destroyed.
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

    // One-off click from a module that is not the autoclicker.
    // Goes through the same floor, so it can never stack into an
    // impossible interval.
    static bool RequestClick(bool right = false) {
        if (s_window && GetForegroundWindow() != s_window) return false;
        return Emit(right);
    }

    static void SetActive(bool on)     { s_active.store(on); }
    static bool IsActive()             { return s_active.load(); }
    static void SetRightButton(bool r) { s_rightButton.store(r); }
    static long long LastGap()         { return s_lastGap.load(); }

    static void SetFloor(int ms)       { s_floorMs.store(ms < 1 ? 1 : ms); }
    static int  GetFloor()             { return s_floorMs.load(); }
    static long long Rejected()        { return s_rejected.load(); }
};
