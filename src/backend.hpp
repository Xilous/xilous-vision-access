#pragma once

#include <cstdint>
#include <memory>

namespace xva {

// The largest value of the HID absolute axes; positions are scaled from 0..1 onto 0..kAxisMax.
inline constexpr std::uint16_t kAxisMax = 32767;

// One absolute-mouse HID report.
struct MouseReport {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t buttons = 0;  // bit 0 is left, bit 1 is right
    std::int8_t wheel = 0;     // positive scrolls up
};

// Where reports go. The engine owns exactly one backend and calls it only from its output thread.
class Backend {
public:
    virtual ~Backend() = default;

    // Returns false once the device can no longer be written to.
    virtual bool send(const MouseReport &report) = 0;
};

// Opens the installed VMulti virtual HID mouse, or returns null when none is found.
std::unique_ptr<Backend> open_vmulti_backend();

// Accepts and discards every report, for running without a driver.
std::unique_ptr<Backend> make_dry_run_backend();

}  // namespace xva
