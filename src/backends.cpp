#include "backend.hpp"

namespace xva {
namespace {

class DryRunBackend final : public Backend {
public:
    bool send(const MouseReport &) override { return true; }
};

}  // namespace

std::unique_ptr<Backend> open_vmulti_backend() {
    // Not implemented yet. Before writing it, read the HID report descriptor of the VMulti build that
    // is actually installed, because the signed builds differ from djpnewton's original: the vendor
    // and product IDs, the report ID of the absolute mouse, the order of its fields, whether it has a
    // wheel, and how reports are wrapped for the control collection all come from that descriptor.
    // The device is then found by enumerating HID interfaces (SetupDiGetClassDevs with the GUID from
    // HidD_GetHidGuid) and matching those IDs, and reports are written to its control collection.
    return nullptr;
}

std::unique_ptr<Backend> make_dry_run_backend() {
    return std::make_unique<DryRunBackend>();
}

}  // namespace xva
