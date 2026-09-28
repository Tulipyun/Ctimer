#include "estimator.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>

void check(bool condition, const char* label);
void runEstimatorTests() {
    using namespace ct;
    auto touching = selectIntervals({{0, 1}, {1, 2}}, 0);
    check(touching.valid && touching.envelope.lower == 1 && touching.envelope.upper == 1,
          "closed intervals retain touching endpoints");
    auto ambiguous = selectIntervals({{-10, 10}, {-10, 10}, {-1, 1}, {5, 6}}, 1);
    check(!ambiguous.valid && ambiguous.ambiguous && ambiguous.regions.size() == 2,
          "disjoint plausible fault-model regions are not collapsed");
    auto insufficient = selectIntervals({{-1, 1}, {0, 2}}, 1);
    check(insufficient.valid && !insufficient.faultToleranceReady && insufficient.toleratedFaults == 0,
          "insufficient source groups explicitly downgrade fault model");
    std::vector<Sample> honest = {
        {0, L"a", 0, 6, 20, .05, 1}, {1, L"b", 0, 6, 20, .05, 1}, {2, L"c", 0, 6, 20, .05, 1}};
    honest.push_back({3, L"bad", 0, 6, 0, .05, 1});
    auto result = combineSources(honest, 1);
    check(result.valid && result.groups == 1 && result.faultToleranceReady,
          "fault counterexample still permits quality-ranked point");
    check(result.uncertaintyMs >= 10.05 - 1e-9 && result.offsetMs - result.uncertaintyMs <= 0,
          "quality pruning cannot shrink fault-model interval");
    std::mt19937 random(6001);
    std::uniform_real_distribution<double> delay(.1, 60), phase(-100, 100);
    bool covered = true;
    unsigned resolved = 0, unresolved = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        const double truth = phase(random);
        std::vector<Sample> samples;
        for (int i = 0; i < 3; ++i) {
            double up = delay(random), down = delay(random);
            samples.push_back({static_cast<std::size_t>(i), std::to_wstring(i), 0, truth + (up - down) / 2,
                               up + down, .05, 1});
        }
        samples.push_back({3, L"fault", 0, phase(random), .01, .05, 1});
        auto estimate = combineSources(samples, 1);
        if (estimate.valid) {
            ++resolved;
            covered = covered && truth >= estimate.offsetMs - estimate.uncertaintyMs - 1e-8 &&
                      truth <= estimate.offsetMs + estimate.uncertaintyMs + 1e-8;
        } else {
            ++unresolved;
            covered = covered && estimate.ambiguous;
        }
    }
    check(covered && resolved > 0, "2000 one-fault generated cases retain true offset or report ambiguity");
    constexpr Tick frequency = 10000000;
    PathTracker tracker;
    for (int i = 0; i < 120; ++i) {
        const bool spike = i % 11 == 0;
        const double noise = (i % 3 - 1) * .02;
        Sample sample{0,
                      L"a",
                      static_cast<Tick>(i) * 10 * frequency,
                      6 + .03 * (i * 10) + noise + (spike ? 40 : 0),
                      spike ? 100.0 : 4.0,
                      .05,
                      1};
        tracker.observe(sample, frequency);
    }
    auto fit = tracker.frequencyFit(frequency);
    check(fit.valid && fit.mature && std::abs(fit.ppm - 30) < .3,
          "robust per-path regression recovers drift despite queue spikes");
    check(fit.errorPpm >= 2 && fit.errorPpm < 5, "frequency margin retains a floor");
    const Tick finalTime = 1190 * frequency;
    auto filtered = tracker.filtered(finalTime, frequency, fit.ppm, fit.errorPpm);
    const double finalTruth = 6 + .03 * 1190;
    check(filtered.valid && std::abs(filtered.sample.offsetMs - finalTruth) < .1,
          "filtered phase projects observations to common QPC epoch");
    auto bounds = sampleInterval(filtered.sample);
    check(bounds.lower <= finalTruth && bounds.upper >= finalTruth,
          "projected phase interval covers known truth");
    check(filtered.lowDelaySamples < filtered.samples, "phase estimate uses low-delay subset");
    auto predicted = filtered.sample.offsetMs + fit.ppm * .060;
    check(std::abs(predicted - (finalTruth + 1.8)) < .1, "60 second holdover predicts known drift");
    auto before = tracker.size();
    check(!tracker.observe(*tracker.latest(), frequency) && tracker.size() == before,
          "duplicate observations do not add evidence");
    check(!tracker.filtered(finalTime + 200 * frequency, frequency, fit.ppm, fit.errorPpm).valid,
          "old phase observations expire");
    PathTracker shortHistory;
    for (int i = 0; i < 6; ++i)
        shortHistory.observe({0, L"a", i * 10 * frequency, 0, 2, .05, 1}, frequency);
    check(!shortHistory.frequencyFit(frequency).valid, "short history does not claim frequency lock");
    PathTracker contradictory;
    contradictory.observe({0, L"a", frequency, 0, .1, .05, 1}, frequency);
    contradictory.observe({0, L"a", 2 * frequency, 20, .1, .05, 1}, frequency);
    check(!contradictory.filtered(2 * frequency, frequency, 0, 30).valid,
          "conflicting same-path intervals are rejected");
    check(syncPhase(true, false, true, true, 0, 181 * Second, 180) == SyncPhase::Tracking,
          "tracking before refinement window");
    check(syncPhase(true, false, true, true, 0, 180 * Second, 180) == SyncPhase::Refining,
          "refinement starts three minutes before target");
    check(syncPhase(true, false, true, true, 0, 61 * Second, 180) == SyncPhase::Refining,
          "refinement remains before freeze");
    check(syncPhase(false, true, true, true, 0, 60 * Second, 180) == SyncPhase::Frozen,
          "freeze overrides refinement");
    check(syncPhase(false, false, true, true, 0, 100 * Second, 180) == SyncPhase::Paused,
          "refinement respects manual pause");
    check(syncPhase(true, false, true, false, 0, 100 * Second, 180) == SyncPhase::Tracking,
          "refinement can be disabled");
    auto sources = defaultSources();
    check(sources[0].host == L"ntp.ntsc.ac.cn" && sources[1].host == L"ntp.cnnic.cn" &&
              sources[2].host == L"cn.pool.ntp.org",
          "requested Chinese sources are discovered first");
    check(operatorGroup({L"ntp.ntsc.ac.cn", L"fake"}) == L"ntsc" &&
              operatorGroup({L"ntp.cnnic.cn", L"fake"}) == L"cnnic" &&
              operatorGroup({L"0.cn.pool.ntp.org", L"fake"}) == L"pool",
          "Chinese source aliases cannot create extra operator votes");
    check(std::any_of(sources.begin(), sources.end(),
                      [](const auto& s) { return s.host == L"ntp.ubuntu.com"; }) &&
              std::any_of(sources.begin(), sources.end(), [](const auto& s) { return s.group == L"netnod"; }),
          "additional operators are available");
    const auto count = sources.size();
    addDiverseSources(sources);
    check(sources.size() == count, "source migration is idempotent");
    std::ofstream report("build/estimator-validation.json");
    report << "{\"fault_trials\":2000,\"resolved\":" << resolved << ",\"ambiguous\":" << unresolved
           << ",\"coverage_passed\":" << (covered ? "true" : "false")
           << ",\"true_rate_ppm\":30,\"estimated_rate_ppm\":" << fit.ppm
           << ",\"rate_margin_ppm\":" << fit.errorPpm
           << ",\"phase_error_ms\":" << filtered.sample.offsetMs - finalTruth
           << ",\"prediction_60s_error_ms\":" << predicted - finalTruth - 1.8 << "}";
}
