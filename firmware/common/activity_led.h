#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include "experiment_config.h"

struct PeakScale {
    float peak=Experiment::led_full_scale;
    void reset(){peak=Experiment::led_full_scale;}
    void observe(float score){if(std::isfinite(score)&&score>peak)peak=score;}
};
// Logical brightness; hardware inversion handles the active-low XIAO LED.
inline unsigned activityBrightness(float score, const char* state, bool waiting,
                                   bool tx, bool running, uint64_t now, float peak=Experiment::led_full_scale) {
    constexpr unsigned full = 1023;
    if (!running) return 0;
    if (tx) return 80;
    if (waiting) return now % 1000000 < 100000 ? 250 : 0;
    if (!std::strcmp(state, "CALIBRATING")) {
        const float phase = (now % 2000000) / 1000000.f;
        return unsigned(50 + 350 * (1 - std::fabs(phase - 1)));
    }
    if (std::strcmp(state, "ACTIVE") && std::strcmp(state, "CLEAR") && std::strcmp(state, "PROVISIONAL")) {
        const uint64_t t = now % 2000000;
        return (t < 100000 || (t >= 250000 && t < 350000)) ? 350 : 0;
    }
    if (!std::isfinite(score)) return 0;
    return unsigned(full * std::clamp(score / std::max(peak,Experiment::led_full_scale), 0.f, 1.f));
}
static_assert(Experiment::led_full_scale > 0, "Positive LED score scale required");
