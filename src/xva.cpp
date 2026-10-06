#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "xva.h"

#include "backend.hpp"
#include "one_euro.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

// Defined by Windows SDK 10.0.17134 and later; spelled out so older SDKs still build.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

// The output thread ticks every 4 ms (250 Hz), a common mouse polling rate.
constexpr LONG kTickPeriodMs = 4;

// Starting values for the One Euro filter in screen fractions; to be tuned with real users.
constexpr double kMinCutoffHz = 1.0;
constexpr double kBeta = 10.0;
constexpr double kDerivativeCutoffHz = 1.0;

// A jump longer than this fraction of the screen is animated rather than taken in one report.
constexpr double kJumpThreshold = 0.05;
constexpr double kJumpDurationSec = 0.08;

struct InputEvent {
    enum class Kind { Button, Scroll };
    Kind kind;
    int button;  // an xva_button value, for Kind::Button
    bool down;   // for Kind::Button
    int notches; // for Kind::Scroll
};

// The minimum-jerk profile: position along a movement whose velocity rises and falls smoothly.
double minimum_jerk(double t) {
    return t * t * t * (10.0 - 15.0 * t + 6.0 * t * t);
}

std::uint16_t to_axis(double fraction) {
    return static_cast<std::uint16_t>(std::lround(std::clamp(fraction, 0.0, 1.0) * xva::kAxisMax));
}

// The OS cursor's position as screen fractions, so the first report does not move it.
void initial_cursor(double &x, double &y) {
    x = 0.5;
    y = 0.5;
    POINT p{};
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (GetCursorPos(&p) && width > 1 && height > 1) {
        x = std::clamp(p.x / static_cast<double>(width - 1), 0.0, 1.0);
        y = std::clamp(p.y / static_cast<double>(height - 1), 0.0, 1.0);
    }
}

}  // namespace

struct xva_ctx {
    std::unique_ptr<xva::Backend> backend;
    std::thread worker;
    HANDLE timer = nullptr;
    HANDLE wake = nullptr;  // auto-reset; set when an event is queued or on shutdown
    std::atomic<bool> stopping{false};
    std::atomic<bool> device_failed{false};

    std::mutex mutex;  // guards the members below
    double gaze_x = 0.0;
    double gaze_y = 0.0;
    Clock::time_point gaze_time{};
    std::uint64_t gaze_seq = 0;
    std::deque<InputEvent> events;
    double cursor_x = 0.0;
    double cursor_y = 0.0;

    ~xva_ctx() {
        if (timer) CloseHandle(timer);
        if (wake) CloseHandle(wake);
    }
};

namespace {

void run_output(xva_ctx *ctx) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    xva::OneEuroFilter filter_x(kMinCutoffHz, kBeta, kDerivativeCutoffHz);
    xva::OneEuroFilter filter_y(kMinCutoffHz, kBeta, kDerivativeCutoffHz);

    double cursor_x, cursor_y;
    {
        std::lock_guard lock(ctx->mutex);
        cursor_x = ctx->cursor_x;
        cursor_y = ctx->cursor_y;
    }
    double target_x = cursor_x;
    double target_y = cursor_y;
    std::uint64_t seen_seq = 0;
    Clock::time_point last_gaze_time{};

    bool animating = false;
    Clock::time_point animation_start{};
    double start_x = 0.0;
    double start_y = 0.0;

    std::uint8_t buttons = 0;
    bool moved_once = false;
    xva::MouseReport last_sent{};

    const HANDLE handles[] = {ctx->wake, ctx->timer};
    std::deque<InputEvent> events;

    while (!ctx->stopping) {
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (ctx->stopping) break;

        double gaze_x, gaze_y;
        Clock::time_point gaze_time;
        std::uint64_t gaze_seq;
        {
            std::lock_guard lock(ctx->mutex);
            events.swap(ctx->events);
            gaze_x = ctx->gaze_x;
            gaze_y = ctx->gaze_y;
            gaze_time = ctx->gaze_time;
            gaze_seq = ctx->gaze_seq;
        }

        if (gaze_seq != seen_seq) {
            const double dt = seen_seq == 0 ? 0.0 : Seconds(gaze_time - last_gaze_time).count();
            target_x = filter_x.filter(gaze_x, dt);
            target_y = filter_y.filter(gaze_y, dt);
            seen_seq = gaze_seq;
            last_gaze_time = gaze_time;
        }

        // Small movements pass straight through, so the user's own fine motion is kept. A long jump
        // is spread over a short animation that keeps heading for the live target.
        const auto now = Clock::now();
        if (!animating && std::hypot(target_x - cursor_x, target_y - cursor_y) > kJumpThreshold) {
            animating = true;
            animation_start = now;
            start_x = cursor_x;
            start_y = cursor_y;
        }
        if (animating) {
            const double t = Seconds(now - animation_start).count() / kJumpDurationSec;
            if (t >= 1.0) {
                animating = false;
                cursor_x = target_x;
                cursor_y = target_y;
            } else {
                const double progress = minimum_jerk(t);
                cursor_x = start_x + (target_x - start_x) * progress;
                cursor_y = start_y + (target_y - start_y) * progress;
            }
        } else {
            cursor_x = target_x;
            cursor_y = target_y;
        }

        xva::MouseReport report;
        report.x = to_axis(cursor_x);
        report.y = to_axis(cursor_y);
        report.buttons = buttons;

        // Each queued event gets its own report, so a press and release inside one tick stay a click.
        bool ok = true;
        if (events.empty()) {
            if (!moved_once || report.x != last_sent.x || report.y != last_sent.y) {
                ok = ctx->backend->send(report);
                moved_once = true;
                last_sent = report;
            }
        } else {
            for (const InputEvent &event : events) {
                if (event.kind == InputEvent::Kind::Button) {
                    const auto bit = static_cast<std::uint8_t>(1u << event.button);
                    buttons = event.down ? static_cast<std::uint8_t>(buttons | bit)
                                         : static_cast<std::uint8_t>(buttons & ~bit);
                    report.buttons = buttons;
                    report.wheel = 0;
                } else {
                    report.wheel = static_cast<std::int8_t>(event.notches);
                }
                if (!ctx->backend->send(report)) {
                    ok = false;
                    break;
                }
            }
            report.wheel = 0;
            moved_once = true;
            last_sent = report;
            events.clear();
        }

        {
            std::lock_guard lock(ctx->mutex);
            ctx->cursor_x = cursor_x;
            ctx->cursor_y = cursor_y;
        }

        if (!ok) {
            ctx->device_failed = true;
            break;
        }
    }
}

xva_status check_usable(xva_ctx *ctx) {
    if (!ctx) return XVA_ERR_BAD_ARG;
    if (ctx->device_failed) return XVA_ERR_DEVICE;
    return XVA_OK;
}

xva_status queue_event(xva_ctx *ctx, const InputEvent &event) {
    try {
        std::lock_guard lock(ctx->mutex);
        ctx->events.push_back(event);
    } catch (...) {
        return XVA_ERR_SYSTEM;
    }
    SetEvent(ctx->wake);
    return XVA_OK;
}

}  // namespace

extern "C" {

XVA_API xva_status xva_open(xva_ctx **out, unsigned flags) {
    if (!out) return XVA_ERR_BAD_ARG;
    *out = nullptr;
    if (flags & ~XVA_OPEN_DRY_RUN) return XVA_ERR_BAD_ARG;

    try {
        auto ctx = std::make_unique<xva_ctx>();

        ctx->backend = (flags & XVA_OPEN_DRY_RUN) ? xva::make_dry_run_backend() : xva::open_vmulti_backend();
        if (!ctx->backend) return XVA_ERR_NO_DRIVER;

        ctx->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // A default timer only fires every 15.6 ms; the high-resolution one keeps the 4 ms tick.
        // It needs Windows 10 1803 or later, so fall back to a default timer before that.
        ctx->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!ctx->timer) ctx->timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!ctx->wake || !ctx->timer) return XVA_ERR_SYSTEM;

        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(kTickPeriodMs) * 10'000;  // relative, in 100 ns units
        if (!SetWaitableTimer(ctx->timer, &due, kTickPeriodMs, nullptr, nullptr, FALSE)) return XVA_ERR_SYSTEM;

        initial_cursor(ctx->cursor_x, ctx->cursor_y);
        ctx->worker = std::thread(run_output, ctx.get());

        *out = ctx.release();
        return XVA_OK;
    } catch (...) {
        return XVA_ERR_SYSTEM;
    }
}

XVA_API void xva_close(xva_ctx *ctx) {
    if (!ctx) return;
    ctx->stopping = true;
    SetEvent(ctx->wake);
    if (ctx->worker.joinable()) ctx->worker.join();
    delete ctx;
}

XVA_API xva_status xva_gaze(xva_ctx *ctx, double x, double y) {
    if (const xva_status status = check_usable(ctx); status != XVA_OK) return status;
    if (!std::isfinite(x) || !std::isfinite(y)) return XVA_ERR_BAD_ARG;

    // No wake-up here: the output thread reads the newest sample on its next tick, so the output
    // rate stays fixed however fast or unevenly samples arrive.
    std::lock_guard lock(ctx->mutex);
    ctx->gaze_x = std::clamp(x, 0.0, 1.0);
    ctx->gaze_y = std::clamp(y, 0.0, 1.0);
    ctx->gaze_time = Clock::now();
    ++ctx->gaze_seq;
    return XVA_OK;
}

XVA_API xva_status xva_button(xva_ctx *ctx, xva_button button, int down) {
    if (const xva_status status = check_usable(ctx); status != XVA_OK) return status;
    if (button != XVA_BUTTON_LEFT && button != XVA_BUTTON_RIGHT) return XVA_ERR_BAD_ARG;
    return queue_event(ctx, {InputEvent::Kind::Button, static_cast<int>(button), down != 0, 0});
}

XVA_API xva_status xva_scroll(xva_ctx *ctx, int notches) {
    if (const xva_status status = check_usable(ctx); status != XVA_OK) return status;
    if (notches < -127 || notches > 127) return XVA_ERR_BAD_ARG;
    if (notches == 0) return XVA_OK;
    return queue_event(ctx, {InputEvent::Kind::Scroll, 0, false, notches});
}

XVA_API xva_status xva_get_cursor(xva_ctx *ctx, double *x, double *y) {
    if (!ctx || !x || !y) return XVA_ERR_BAD_ARG;
    std::lock_guard lock(ctx->mutex);
    *x = ctx->cursor_x;
    *y = ctx->cursor_y;
    return XVA_OK;
}

XVA_API const char *xva_status_string(xva_status status) {
    switch (status) {
    case XVA_OK: return "ok";
    case XVA_ERR_BAD_ARG: return "invalid argument";
    case XVA_ERR_NO_DRIVER: return "no virtual HID mouse driver is installed";
    case XVA_ERR_DEVICE: return "the virtual mouse stopped accepting reports";
    case XVA_ERR_SYSTEM: return "a Windows resource could not be created";
    }
    return "unknown status";
}

}  // extern "C"
