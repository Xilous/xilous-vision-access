#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <hidsdi.h>
#include <setupapi.h>

#include "vmulti_device.hpp"

#include <vector>

namespace xva {
namespace {

bool is_control_collection(const wchar_t *path) {
    // Zero access is enough to read attributes and capabilities, and it also works on the devices
    // Windows holds exclusively, such as real keyboards and mice.
    const HANDLE probe = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return false;

    bool match = false;
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (HidD_GetAttributes(probe, &attributes) && attributes.VendorID == kVMultiVendorId &&
        attributes.ProductID == kVMultiProductId && HidD_GetPreparsedData(probe, &preparsed)) {
        HIDP_CAPS caps{};
        match = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == kVMultiControlUsagePage &&
                caps.Usage == kVMultiControlUsage && caps.OutputReportByteLength == kVMultiControlReportSize;
        HidD_FreePreparsedData(preparsed);
    }
    CloseHandle(probe);
    return match;
}

}  // namespace

HANDLE open_vmulti_control(DWORD access) {
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
            found = CreateFileW(detail->DevicePath, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

}  // namespace xva
