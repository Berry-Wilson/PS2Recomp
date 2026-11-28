#include "runtime/display_clock.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#endif

namespace ps2x::display_clock
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The guest's vertical blank goes this long after the display's refresh, so the timer's wake-up
        // jitter (about a millisecond) never puts it before the refresh it belongs to.
        constexpr auto kAfterRefresh = std::chrono::microseconds(500);
        // A refresh time older than this is not trusted (the display is off, or the thread stalled).
        constexpr auto kStale = std::chrono::milliseconds(250);
        // How far from 60 or 120 Hz the display may be and still be followed: 59.94 is 0.1% off. Wider
        // than real displays need, because where the driver has no mailbox presentation PCSX2 uses
        // FIFO, and a display just under 60 Hz that is not followed would then hold the game back with
        // up to three frames queued.
        constexpr double kTolerance = 0.02;
        // Followed at 60 and 120 Hz only. A frame reaches the screen at the first refresh after PCSX2 has
        // drawn it, and how long that takes varies by a few milliseconds. At 240 Hz a refresh is shorter
        // than that variation, so a fixed place in the refresh cannot keep frames off the boundary, and
        // pinning them there was worse in simulation than the drifting 60 Hz clock.
        constexpr double kMaxMultiple = 2.0;
        // The refreshes of the last two seconds are fitted: the wake-up after each one is off by up to
        // half a millisecond, which over fewer refreshes on a fast display makes the rate wander.
        constexpr auto kWindow = std::chrono::seconds(2);
        constexpr size_t kMaxSamples = 2048;
        constexpr size_t kMinSamples = 30;

        struct Shared
        {
            std::mutex lock;
            bool locked = false;
            Clock::time_point refresh{}; // a recent display refresh, from the fit
            Clock::duration period{};
        };
        Shared s_shared;

        std::mutex s_threadLock;
        std::condition_variable s_threadDone;
        std::atomic<bool> s_stop{false};
        bool s_running = false;
        std::thread s_thread;

        void setUnlocked()
        {
            std::lock_guard<std::mutex> lock(s_shared.lock);
            s_shared.locked = false;
        }

        // Fits refresh times to a straight line: refresh k at t0 + k * period. Missed refreshes (the
        // thread woke late) only leave gaps in k. Returns false while there is too little to go on.
        bool fit(const std::vector<Clock::time_point> &samples, double &periodNs, Clock::time_point &last)
        {
            if (samples.size() < kMinSamples)
                return false;
            std::vector<double> gaps;
            gaps.reserve(samples.size());
            for (size_t i = 1; i < samples.size(); ++i)
                gaps.push_back(std::chrono::duration<double, std::nano>(samples[i] - samples[i - 1]).count());
            std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
            const double median = gaps[gaps.size() / 2];
            if (median <= 0.0)
                return false;
            double sk = 0, st = 0, skk = 0, skt = 0;
            const double n = static_cast<double>(samples.size());
            std::vector<double> ks(samples.size()), ts(samples.size());
            for (size_t i = 0; i < samples.size(); ++i)
            {
                ts[i] = std::chrono::duration<double, std::nano>(samples[i] - samples.front()).count();
                ks[i] = std::round(ts[i] / median);
                sk += ks[i];
                st += ts[i];
                skk += ks[i] * ks[i];
                skt += ks[i] * ts[i];
            }
            const double denom = n * skk - sk * sk;
            if (denom <= 0.0)
                return false;
            periodNs = (n * skt - sk * st) / denom;
            const double intercept = (st - periodNs * sk) / n;
            const double lastNs = intercept + periodNs * ks.back();
            last = samples.front() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::nano>(lastNs));
            return periodNs > 0.0;
        }

#if defined(_WIN32)
        IDXGIOutput *findOutput(IDXGIFactory1 *factory, HMONITOR monitor)
        {
            IDXGIAdapter1 *adapter = nullptr;
            for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
            {
                IDXGIOutput *output = nullptr;
                for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
                {
                    DXGI_OUTPUT_DESC desc{};
                    if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                    {
                        adapter->Release();
                        return output;
                    }
                    output->Release();
                }
                adapter->Release();
            }
            return nullptr;
        }

        void run(HWND window)
        {
            // Woken once a refresh for a moment; running ahead of the game's threads keeps the wake-up close.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            IDXGIFactory1 *factory = nullptr;
            if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
            {
                std::fprintf(stderr, "[display] no DXGI factory; the game keeps its own 60 Hz clock\n");
                return;
            }
            HMONITOR monitor = nullptr;
            IDXGIOutput *output = nullptr;
            std::vector<Clock::time_point> samples;
            Clock::time_point lastMonitorCheck{};
            int reportedLock = -1;
            double reportedHz = 0.0;
            auto dropOutput = [&]()
            {
                if (output)
                    output->Release();
                output = nullptr;
                samples.clear();
                setUnlocked();
            };

            while (!s_stop.load(std::memory_order_acquire))
            {
                const Clock::time_point now = Clock::now();
                // The window may have moved to another display, or gone full screen on one.
                if (now - lastMonitorCheck >= std::chrono::seconds(1))
                {
                    lastMonitorCheck = now;
                    const HMONITOR current = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
                    if (current != monitor || !output)
                    {
                        dropOutput();
                        monitor = current;
                        output = findOutput(factory, monitor);
                    }
                }
                if (!output)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
                if (FAILED(output->WaitForVBlank()))
                {
                    dropOutput();
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
                const Clock::time_point t = Clock::now();
                // A display that is off can return at once; that is no refresh.
                if (!samples.empty() && t - samples.back() < std::chrono::milliseconds(1))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                samples.push_back(t);
                size_t drop = 0;
                while (drop < samples.size() && t - samples[drop] > kWindow)
                    ++drop;
                drop = std::max(drop, samples.size() > kMaxSamples ? samples.size() - kMaxSamples : size_t{0});
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(drop));

                double periodNs = 0.0;
                Clock::time_point last{};
                if (!fit(samples, periodNs, last))
                    continue;
                const double hz = 1e9 / periodNs;
                const double multiple = std::round(hz / 60.0);
                const bool locked = multiple >= 1.0 && multiple <= kMaxMultiple && std::fabs(hz - 60.0 * multiple) <= kTolerance * 60.0 * multiple;
                {
                    std::lock_guard<std::mutex> lock(s_shared.lock);
                    s_shared.locked = locked;
                    s_shared.refresh = last;
                    s_shared.period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::nano>(periodNs));
                }
                if (static_cast<int>(locked) != reportedLock || std::fabs(hz - reportedHz) > 0.01 * reportedHz)
                {
                    std::fprintf(stderr, locked ? "[display] %.2f Hz: the game's frames follow the display's refreshes\n"
                                                : "[display] %.2f Hz: the game keeps its own 60 Hz clock\n",
                                 hz);
                    reportedLock = static_cast<int>(locked);
                    reportedHz = hz;
                }
            }
            dropOutput();
            factory->Release();
        }
#endif
    }

    bool vsyncEnabled()
    {
        const char *value = std::getenv("PS2X_VSYNC");
        return !(value && std::strcmp(value, "0") == 0);
    }

    void start(void *nativeWindow)
    {
#if defined(_WIN32)
        std::lock_guard<std::mutex> lock(s_threadLock);
        if (s_running || !nativeWindow || !vsyncEnabled())
            return;
        s_stop.store(false, std::memory_order_release);
        s_running = true;
        s_thread = std::thread([window = static_cast<HWND>(nativeWindow)]()
                               {
                                   run(window);
                                   std::lock_guard<std::mutex> done(s_threadLock);
                                   s_running = false;
                                   s_threadDone.notify_all();
                               });
#else
        (void)nativeWindow;
#endif
    }

    void stop()
    {
        setUnlocked();
        std::unique_lock<std::mutex> lock(s_threadLock);
        if (!s_thread.joinable())
            return;
        s_stop.store(true, std::memory_order_release);
        // WaitForVBlank returns within a refresh; a display that has gone away might not, and closing the
        // game must not hang on it.
        if (s_threadDone.wait_for(lock, std::chrono::milliseconds(500), [] { return !s_running; }))
        {
            lock.unlock();
            s_thread.join();
        }
        else
        {
            s_thread.detach();
        }
    }

    std::chrono::steady_clock::time_point align(std::chrono::steady_clock::time_point wanted,
                                                std::chrono::steady_clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(s_shared.lock);
        if (!s_shared.locked || now - s_shared.refresh > kStale || s_shared.period.count() <= 0)
            return wanted;
        const Clock::duration period = s_shared.period;
        const double steps = static_cast<double>((wanted - s_shared.refresh).count()) / static_cast<double>(period.count());
        Clock::time_point t = s_shared.refresh + period * static_cast<long long>(std::llround(steps)) + kAfterRefresh;
        if (t < now)
            t += period * ((now - t) / period + 1);
        return t;
    }
}
