#pragma once

#include <windows.h>

namespace xva {

// The control collection of the signed VMulti build XVA targets (X9VoiD/vmulti-bin v1.0,
// hardware ID pentablet\hid), as read from its driver: vendor 0x00FF, product 0xBACC, and a
// vendor-defined collection (usage page 0xFF00, usage 0x01) taking 65-byte output reports.
inline constexpr USHORT kVMultiVendorId = 0x00FF;
inline constexpr USHORT kVMultiProductId = 0xBACC;
inline constexpr USHORT kVMultiControlUsagePage = 0xFF00;
inline constexpr USHORT kVMultiControlUsage = 0x0001;
inline constexpr DWORD kVMultiControlReportSize = 65;

// Opens the VMulti control collection with the given access (0 only checks it is there), or
// returns INVALID_HANDLE_VALUE when no working VMulti device is present.
HANDLE open_vmulti_control(DWORD access);

}  // namespace xva
