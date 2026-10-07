#include "backend.hpp"

namespace xva {
namespace {

class DryRunBackend final : public Backend {
public:
    bool send(const MouseReport &) override { return true; }
};

}  // namespace

std::unique_ptr<Backend> make_dry_run_backend() {
    return std::make_unique<DryRunBackend>();
}

}  // namespace xva
