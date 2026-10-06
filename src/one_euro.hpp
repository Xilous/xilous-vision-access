#pragma once

#include <cmath>
#include <numbers>

namespace xva {

// The One Euro filter (Casiez, Roussel and Vogel, CHI 2012): a low-pass filter whose cutoff rises
// with speed. A gaze that is nearly still is smoothed heavily, so the user can aim at small targets,
// while a fast movement is followed with little lag. It reduces the tracker's jitter without
// replacing it: what remains is still the user's own movement.
class OneEuroFilter {
public:
    OneEuroFilter(double min_cutoff_hz, double beta, double derivative_cutoff_hz)
        : min_cutoff_hz_(min_cutoff_hz), beta_(beta), derivative_cutoff_hz_(derivative_cutoff_hz) {}

    // Filters one sample taken dt seconds after the previous one.
    double filter(double x, double dt) {
        if (!initialized_) {
            initialized_ = true;
            x_prev_ = x;
            dx_prev_ = 0.0;
            return x;
        }
        if (dt <= 0.0) {
            return x_prev_;
        }
        const double dx = (x - x_prev_) / dt;
        dx_prev_ += alpha(derivative_cutoff_hz_, dt) * (dx - dx_prev_);
        const double cutoff_hz = min_cutoff_hz_ + beta_ * std::abs(dx_prev_);
        x_prev_ += alpha(cutoff_hz, dt) * (x - x_prev_);
        return x_prev_;
    }

private:
    static double alpha(double cutoff_hz, double dt) {
        const double tau = 1.0 / (2.0 * std::numbers::pi * cutoff_hz);
        return 1.0 / (1.0 + tau / dt);
    }

    double min_cutoff_hz_;
    double beta_;
    double derivative_cutoff_hz_;
    bool initialized_ = false;
    double x_prev_ = 0.0;
    double dx_prev_ = 0.0;
};

}  // namespace xva
