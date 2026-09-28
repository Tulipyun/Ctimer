#pragma once
#include "core.hpp"
#include <deque>

namespace ct {
struct FrequencyFit {
    bool valid{}, mature{};
    double ppm{}, errorPpm{}, residualMs{}, spanSeconds{};
    std::size_t samples{};
};
struct FilteredSource {
    bool valid{};
    Sample sample;
    std::size_t samples{}, lowDelaySamples{};
    double basisAgeSeconds{};
    std::wstring reason;
};
FrequencyFit robustFrequencyFit(const std::vector<Sample>& samples, Tick frequency);
class PathTracker {
  public:
    bool observe(const Sample& sample, Tick frequency);
    const Sample* latest() const;
    FrequencyFit frequencyFit(Tick frequency) const;
    FilteredSource filtered(Tick now, Tick frequency, double ppm, double errorPpm, int windowSeconds = 120,
                            Tick refinementStart = 0) const;
    std::size_t size() const { return history_.size(); }

  private:
    std::deque<Sample> history_;
    mutable std::uint64_t revision_{}, fittedRevision_{~std::uint64_t{0}};
    mutable FrequencyFit cachedFit_;
};
} // namespace ct
