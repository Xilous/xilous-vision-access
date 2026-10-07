#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <hidsdi.h>
#include <setupapi.h>

#include "backend.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace xva {
namespace {

// These values come from the device attributes and report descriptor compiled into the signed
// vmulti.sys that X9VoiD/vmulti-bin distributes (catalog pentablethid.cat, hardware ID pentablet\hid).
// Other VMulti builds use other report IDs and layouts.
constexpr USHORT kVendorId = 0x00FF;
constexpr USHORT kProductId = 0xBACC;
constexpr USHORT kControlUsagePage = 0xFF00;
constexpr USHORT kControlUsage = 0x0001;
constexpr DWORD kControlReportSize = 65;  // report ID 0x40 followed by 64 bytes

// A control report is: 0x40, the inner report's length (counting its own ID), then the inner report.
constexpr BYTE kControlReportId = 0x40;
// Absolute mouse: ID, buttons (bits 0-2), X and Y as 16-bit 0..32767, a 16-bit pressure field. No wheel.
constexpr BYTE kAbsoluteMouseReportId = 0x09;
constexpr BYTE kAbsoluteMouseReportLength = 8;
// Relative mouse: ID, buttons (bits 0-4), X, Y and wheel as signed bytes.
constexpr BYTE kRelativeMouseReportId = 0x04;
constexpr BYTE kRelativeMouseReportLength = 5;

using Report = std::array<BYTE, kControlReportSize>;

bool is_control_collection(const wchar_t *path) {
    // Zero access is enough to read attributes and capabilities, and it also works on the devices
    // Windows holds exclusively, such as real keyboards and mice.
    const HANDLE probe = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return false;

    bool match = false;
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (HidD_GetAttributes(probe, &attributes) && attributes.VendorID == kVendorId &&
        attributes.ProductID == kProductId && HidD_GetPreparsedData(probe, &preparsed)) {
        HIDP_CAPS caps{};
        match = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == kControlUsagePage &&
                caps.Usage == kControlUsage && caps.OutputReportByteLength == kControlReportSize;
        HidD_FreePreparsedData(preparsed);
    }
    CloseHandle(probe);
    return match;
}

HANDLE open_control_collection() {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    const HDEVINFO devices = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE &&
                      SetupDiEnumDeviceInterfaces(devices, nullptr, &hid_guid, i, &interface_data);
         ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> buffer(needed);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, needed, nullptr, nullptr)) continue;
        if (is_control_collection(detail->DevicePath)) {
            found = CreateFileW(detail->DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

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
    const HANDLE device = open_control_collection();
    if (device == INVALID_HANDLE_VALUE) return nullptr;
    return std::make_unique<VMultiBackend>(device);
}

}  // namespace xva
