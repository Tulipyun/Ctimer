#include "estimator.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ct {
namespace {
double mad(const std::vector<double>& values) {
    const double center = median(values);
    std::vector<double> deviations;
    for (double value : values)
        deviations.push_back(std::abs(value - center));
    return 1.4826 * median(deviations);
}
struct Point {
    double x, y, weight;
};
std::optional<double> weightedSlope(const std::vector<Point>& points, std::size_t begin, std::size_t end) {
    double weight = 0, x = 0, y = 0;
    for (auto i = begin; i < end; ++i) {
        weight += points[i].weight;
        x += points[i].weight * points[i].x;
        y += points[i].weight * points[i].y;
    }
    if (weight <= 0)
        return {};
    x /= weight;
    y /= weight;
    double xx = 0, xy = 0;
    for (auto i = begin; i < end; ++i) {
        const auto dx = points[i].x - x;
        xx += points[i].weight * dx * dx;
        xy += points[i].weight * dx * (points[i].y - y);
    }
    if (xx <= 1e-12)
        return {};
    return xy / xx;
}
} // namespace
FrequencyFit robustFrequencyFit(const std::vector<Sample>& samples, Tick frequency) {
    FrequencyFit fit;
    if (samples.size() < 8 || frequency <= 0)
        return fit;
    std::vector<double> rtts;
    for (const auto& sample : samples)
        rtts.push_back(sample.rttMs);
    std::sort(rtts.begin(), rtts.end());
    const double cutoff = rtts[(rtts.size() - 1) * 3 / 5] + std::max(0.2, mad(rtts));
    std::vector<Point> points;
    const Tick origin = samples.back().qpc;
    for (const auto& sample : samples) {
        if (sample.rttMs > cutoff)
            continue;
        const double radius = std::max(0.05, sample.rttMs / 2 + sample.rootDistanceMs);
        points.push_back(
            {static_cast<double>(sample.qpc - origin) / frequency, sample.offsetMs, 1 / (radius * radius)});
    }
    if (points.size() < 8)
        return fit;
    fit.samples = points.size();
    fit.spanSeconds = points.back().x - points.front().x;
    if (fit.spanSeconds < 120)
        return fit;
    std::vector<double> slopes;
    for (std::size_t i = 0; i < points.size(); ++i)
        for (std::size_t j = i + 1; j < points.size(); ++j) {
            double span = points[j].x - points[i].x;
            if (span >= 30)
                slopes.push_back((points[j].y - points[i].y) / span);
        }
    if (slopes.empty())
        return fit;
    double slope = median(slopes), intercept = 0;
    std::vector<double> offsets;
    for (const auto& point : points)
        offsets.push_back(point.y - slope * point.x);
    intercept = median(offsets);
    for (int iteration = 0; iteration < 5; ++iteration) {
        std::vector<double> residuals;
        for (const auto& point : points)
            residuals.push_back(point.y - intercept - slope * point.x);
        const double scale = std::max(0.02, mad(residuals));
        auto robust = points;
        for (std::size_t i = 0; i < points.size(); ++i)
            robust[i].weight *= std::min(1.0, 2.5 * scale / std::max(1e-12, std::abs(residuals[i])));
        auto estimate = weightedSlope(robust, 0, robust.size());
        if (!estimate)
            return fit;
        slope = *estimate;
        double sum = 0, weights = 0;
        for (const auto& point : robust) {
            sum += point.weight * (point.y - slope * point.x);
            weights += point.weight;
        }
        intercept = sum / weights;
    }
    std::vector<double> residuals;
    for (const auto& point : points)
        residuals.push_back(point.y - intercept - slope * point.x);
    fit.residualMs = std::max(0.02, mad(residuals));
    fit.ppm = slope * 1000;
    // Engineering allowance: do not divide correlated observations by sqrt(N).
    fit.errorPpm = std::max(2.0, 3 * fit.residualMs / fit.spanSeconds * 1000);
    const auto middle = points.size() / 2;
    if (auto first = weightedSlope(points, 0, middle))
        fit.errorPpm = std::max(fit.errorPpm, std::abs(*first - slope) * 1000);
    if (auto last = weightedSlope(points, middle, points.size()))
        fit.errorPpm = std::max(fit.errorPpm, std::abs(*last - slope) * 1000);
    fit.valid = std::isfinite(fit.ppm) && std::isfinite(fit.errorPpm) && std::abs(fit.ppm) <= 200 &&
                fit.errorPpm <= 200;
    fit.mature = fit.valid && fit.samples >= 16 && fit.spanSeconds >= 300;
    return fit;
}
bool PathTracker::observe(const Sample& sample, Tick frequency) {
    if (frequency <= 0 || !std::isfinite(sample.offsetMs) || !std::isfinite(sample.rttMs) ||
        !std::isfinite(sample.rootDistanceMs) || sample.rttMs < 0 || sample.rootDistanceMs < 0)
        return false;
    if (!history_.empty() && sample.qpc <= history_.back().qpc)
        return false;
    if (!history_.empty() && sample.qpc - history_.back().qpc > 300 * frequency)
        history_.clear();
    while (!history_.empty() && sample.qpc - history_.front().qpc > 1800 * frequency)
        history_.pop_front();
    history_.push_back(sample);
    if (history_.size() > 256)
        history_.pop_front();
    ++revision_;
    return true;
}
const Sample* PathTracker::latest() const {
    return history_.empty() ? nullptr : &history_.back();
}
FrequencyFit PathTracker::frequencyFit(Tick frequency) const {
    if (fittedRevision_ != revision_) {
        cachedFit_ = robustFrequencyFit({history_.begin(), history_.end()}, frequency);
        fittedRevision_ = revision_;
    }
    return cachedFit_;
}
FilteredSource PathTracker::filtered(Tick now, Tick frequency, double ppm, double errorPpm, int windowSeconds,
                                     Tick refinementStart) const {
    FilteredSource result;
    if (history_.empty() || frequency <= 0 || errorPpm < 0)
        return result;
    std::vector<Sample> recent;
    const Tick oldest = now - static_cast<Tick>(windowSeconds) * frequency;
    for (const auto& sample : history_)
        if (sample.qpc >= oldest && sample.qpc <= now && (!refinementStart || sample.qpc >= refinementStart))
            recent.push_back(sample);
    if (recent.size() < 3 && refinementStart) {
        recent.clear();
        for (const auto& sample : history_)
            if (sample.qpc >= oldest && sample.qpc <= now)
                recent.push_back(sample);
    }
    if (recent.empty()) {
        result.reason = L"样本过期";
        return result;
    }
    struct Projected {
        Sample raw;
        double offset, radius;
    };
    std::vector<Projected> projected;
    Interval bounds{-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    std::vector<double> phases;
    for (const auto& sample : recent) {
        const double age = static_cast<double>(now - sample.qpc) / frequency;
        const double offset = sample.offsetMs + age * ppm / 1000;
        const double radius =
            std::max(0.05, sample.rttMs / 2 + sample.rootDistanceMs) + age * errorPpm / 1000;
        bounds.lower = std::max(bounds.lower, offset - radius);
        bounds.upper = std::min(bounds.upper, offset + radius);
        projected.push_back({sample, offset, radius});
        phases.push_back(offset);
    }
    if (bounds.lower > bounds.upper) {
        result.reason = L"路径样本区间冲突";
        return result;
    }
    std::stable_sort(projected.begin(), projected.end(),
                     [](const auto& a, const auto& b) { return a.radius < b.radius; });
    const auto count = std::min<std::size_t>(
        8, std::min(projected.size(), std::max<std::size_t>(3, (projected.size() + 3) / 4)));
    std::vector<double> lowOffsets, lowRtt, roots;
    Tick basis = 0;
    for (std::size_t i = 0; i < count; ++i) {
        lowOffsets.push_back(projected[i].offset);
        lowRtt.push_back(projected[i].raw.rttMs);
        roots.push_back(projected[i].raw.rootDistanceMs);
        basis = std::max(basis, projected[i].raw.qpc);
    }
    result.sample = recent.back();
    result.sample.offsetMs = std::clamp(median(lowOffsets), bounds.lower, bounds.upper);
    result.sample.rttMs = median(lowRtt);
    result.sample.rootDistanceMs = median(roots);
    result.sample.interval = bounds;
    result.sample.jitterMs = mad(phases);
    result.samples = recent.size();
    result.lowDelaySamples = count;
    result.basisAgeSeconds = static_cast<double>(now - basis) / frequency;
    result.valid = true;
    return result;
}
} // namespace ct
