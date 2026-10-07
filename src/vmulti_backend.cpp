#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "backend.hpp"
#include "vmulti_device.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

namespace xva {
namespace {

// The report layout below comes from the report descriptor compiled into the signed vmulti.sys that
// X9VoiD/vmulti-bin distributes. Other VMulti builds use other report IDs and layouts.
constexpr DWORD kControlReportSize = kVMultiControlReportSize;  // report ID 0x40 followed by 64 bytes

// A control report is: 0x40, the inner report's length (counting its own ID), then the inner report.
constexpr BYTE kControlReportId = 0x40;
// Absolute mouse: ID, buttons (bits 0-2), X and Y as 16-bit 0..32767, a 16-bit pressure field. No wheel.
constexpr BYTE kAbsoluteMouseReportId = 0x09;
constexpr BYTE kAbsoluteMouseReportLength = 8;
// Relative mouse: ID, buttons (bits 0-4), X, Y and wheel as signed bytes.
constexpr BYTE kRelativeMouseReportId = 0x04;
constexpr BYTE kRelativeMouseReportLength = 5;

using Report = std::array<BYTE, kControlReportSize>;

BOOL CALLBACK add_monitor(HMONITOR, HDC, LPRECT rect, LPARAM data) {
    UnionRect(reinterpret_cast<RECT *>(data), reinterpret_cast<RECT *>(data), rect);
    return TRUE;
}

// Converts fractions of the primary monitor into fractions of the virtual desktop, the space an
// absolute HID mouse addresses. Measured in physical pixels from a per-monitor DPI-aware thread
// context, so the host process's own DPI awareness cannot distort it.
class DesktopMap {
public:
    std::pair<double, double> map(double x, double y) {
        const auto now = std::chrono::steady_clock::now();
        if (now - measured_ >= std::chrono::seconds(1)) {
            measure();
            measured_ = now;
        }
        const double px = primary_.left + x * (primary_.right - primary_.left - 1);
        const double py = primary_.top + y * (primary_.bottom - primary_.top - 1);
        return {(px - desktop_.left) / std::max<LONG>(desktop_.right - desktop_.left - 1, 1),
                (py - desktop_.top) / std::max<LONG>(desktop_.bottom - desktop_.top - 1, 1)};
    }

private:
    void measure() {
        const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        RECT desktop{};
        if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info) &&
            EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&desktop)) &&
            !IsRectEmpty(&desktop)) {
            primary_ = info.rcMonitor;
            desktop_ = desktop;
        }
        if (previous) SetThreadDpiAwarenessContext(previous);
    }

    std::chrono::steady_clock::time_point measured_{};
    RECT primary_{0, 0, 2, 2};
    RECT desktop_{0, 0, 2, 2};
};

class VMultiBackend final : public Backend {
public:
    explicit VMultiBackend(HANDLE device) : device_(device) {}

    ~VMultiBackend() override {
        // Never leave a button held down: a user who cannot click could not release it.
        if (last_.buttons != 0) {
            MouseReport released = last_;
            released.buttons = 0;
            released.wheel = 0;
            send(released);
        }
        CloseHandle(device_);
    }

    bool send(const MouseReport &report) override {
        const auto [x, y] = map_.map(report.x / static_cast<double>(kAxisMax), report.y / static_cast<double>(kAxisMax));

        Report absolute{};
        absolute[0] = kControlReportId;
        absolute[1] = kAbsoluteMouseReportLength;
        absolute[2] = kAbsoluteMouseReportId;
        absolute[3] = static_cast<BYTE>(report.buttons & 0x07);
        put_axis(absolute, 4, x);
        put_axis(absolute, 6, y);
        if (!write(absolute)) return false;
        last_ = report;

        if (report.wheel != 0) {
            // The wheel exists only on the relative mouse. Its buttons stay 0: Windows tracks each
            // mouse's buttons separately, so repeating held buttons here would read as a second press.
            Report relative{};
            relative[0] = kControlReportId;
            relative[1] = kRelativeMouseReportLength;
            relative[2] = kRelativeMouseReportId;
            relative[6] = static_cast<BYTE>(report.wheel);
            if (!write(relative)) return false;
        }
        return true;
    }

private:
    static void put_axis(Report &report, size_t offset, double fraction) {
        const auto value = static_cast<std::uint16_t>(std::lround(std::clamp(fraction, 0.0, 1.0) * kAxisMax));
        report[offset] = static_cast<BYTE>(value & 0xFF);
        report[offset + 1] = static_cast<BYTE>(value >> 8);
    }

    bool write(const Report &report) {
        DWORD written = 0;
        return WriteFile(device_, report.data(), kControlReportSize, &written, nullptr) && written == kControlReportSize;
    }

    HANDLE device_;
    DesktopMap map_;
    MouseReport last_{};
};

}  // namespace

std::unique_ptr<Backend> open_vmulti_backend() {
    const HANDLE device = open_vmulti_control(GENERIC_WRITE);
    if (device == INVALID_HANDLE_VALUE) return nullptr;
    return std::make_unique<VMultiBackend>(device);
}

}  // namespace xva
