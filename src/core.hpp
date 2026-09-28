#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ct {
using Ns = std::int64_t;
using Tick = std::int64_t;
constexpr Ns Second = 1'000'000'000;
constexpr Ns Millisecond = 1'000'000;
constexpr Ns FreezeLead = 60 * Second;
constexpr Ns Hour = 3600 * Second;

struct ClockModel {
    Tick anchorQpc{};
    Ns anchorUtc{};
    double frequencyPpm{};
    double errorMs{};
    double residualPpm{30.0}; // Engineering allowance, not a calibrated hard bound.
    bool calibrated{};
    std::uint64_t version{};
    Ns utc(Tick q, Tick frequency) const;
    Tick deadline(Ns t, Tick frequency) const;
    double uncertainty(Tick q, Tick frequency) const;
};

struct TimeOfDay {
    int hour{-1}, minute{}, second{}, millisecond{};
};
std::optional<TimeOfDay> parseTime(const std::wstring& text);
std::optional<std::wstring> normalizeTimeFields(std::wstring minutes, std::wstring seconds,
                                                std::wstring milliseconds);
Ns nextHourlyTarget(Ns previous, Ns now);
bool freezeDue(Ns now, Ns target);
struct AutoScheduleDraft {
    bool pending{};
    Tick readyAfter{};
    Ns hourReference{};
    void changed(Tick now, Tick frequency, Ns reference) {
        pending = true;
        readyAfter = now + frequency;
        hourReference = reference;
    }
    bool ready(Tick now) const { return pending && now >= readyAfter; }
    void cancel() { pending = false; }
};

struct Source {
    std::wstring host;
    std::wstring group;
    int port{123};
    int minPollSeconds{10};
    bool enabled{true};
};
enum class SyncPhase { Discovering, Tracking, Refining, Frozen, Paused };
SyncPhase syncPhase(bool syncing, bool frozen, bool active, bool refiningEnabled, Ns now, Ns target,
                    int refinementLeadSeconds);
std::vector<Source> defaultSources();
std::vector<Source> regionalSources();
std::vector<Source> diverseSources();
std::wstring operatorGroup(const Source& source);
bool addRegionalSources(std::vector<Source>& sources);
bool addDiverseSources(std::vector<Source>& sources);
std::wstring serializeSources(const std::vector<Source>& sources);
std::optional<std::vector<Source>> parseSources(const std::wstring& text, std::wstring& error);

struct Interval {
    double lower{}, upper{};
};
struct IntervalConsensus {
    bool valid{}, ambiguous{}, faultToleranceReady{};
    int requestedFaults{}, toleratedFaults{}, quorum{};
    Interval envelope;
    std::vector<Interval> regions;
};
IntervalConsensus selectIntervals(const std::vector<Interval>& intervals, int requestedFaults);
struct Sample {
    std::size_t source{};
    std::wstring group;
    Tick qpc{};
    double offsetMs{}, rttMs{}, rootDistanceMs{};
    int stratum{};
    double jitterMs{}, failurePenaltyMs{};
    std::optional<Interval> interval{};
};
Interval sampleInterval(const Sample& sample);
double sampleQuality(const Sample& sample);
struct Estimate {
    bool valid{};
    double offsetMs{}, uncertaintyMs{}, jitterMs{};
    int groups{}, availableGroups{};
    int consensusGroups{};
    std::size_t referenceSource{};
    std::vector<std::size_t> usedSources;
    Interval safeInterval;
    bool faultToleranceReady{}, ambiguous{};
    int toleratedFaults{}, quorum{};
};
Estimate combineSources(const std::vector<Sample>& samples, int requestedFaults = 1);
double median(std::vector<double> v);
struct PhasePoint {
    double seconds{}, offsetMs{};
};
std::optional<double> fitFrequency(const std::vector<PhasePoint>& points);
} // namespace ct
