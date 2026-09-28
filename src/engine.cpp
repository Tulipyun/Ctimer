#include "engine.hpp"
#include "version.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <iomanip>
#include <set>

namespace ct {
static std::wstring sourceKey(const Source& s) {
    return s.host + L":" + std::to_wstring(s.port);
}
Engine::Engine(Config config, std::filesystem::path logs,
               std::function<SystemClockResult(const ClockModel&)> clockSetter)
    : baseline(initialClock()), frequency(qpcFrequency()), config_(std::move(config)),
      logDirectory_(std::move(logs)), clockSetter_(std::move(clockSetter)) {
    state_.clock = baseline;
    sessionId_ = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(baseline.anchorQpc);
    state_.requestedFaults = config_.faultBudget;
    state_.clock.residualPpm = config_.driftAllowancePpm;
    state_.syncing = config_.autoSync;
    manualPaused_ = !config_.autoSync;
    for (auto& s : config_.sources) {
        SourceView v;
        v.source = s;
        state_.sources.push_back(v);
    }
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    networkThread_ = std::jthread([this] { networkLoop(); });
    schedulerThread_ = std::jthread([this] { schedulerLoop(); });
}
Engine::~Engine() {
    shutdown();
    if (wake_)
        CloseHandle(wake_);
    if (cancel_)
        CloseHandle(cancel_);
}
void Engine::shutdown() {
    if (quitting_.exchange(true))
        return;
    SetEvent(cancel_);
    SetEvent(wake_);
    if (networkThread_.joinable())
        networkThread_.join();
    if (schedulerThread_.joinable())
        schedulerThread_.join();
}
Snapshot Engine::snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}
bool Engine::startSync(std::wstring& error) {
    std::scoped_lock lock(mutex_);
    maybeFreezeLocked(qpc());
    if (state_.active && state_.frozen) {
        error = L"已进入执行前 60 秒冻结窗口，完成或取消任务后才能恢复同步。";
        return false;
    }
    if (!std::any_of(state_.sources.begin(), state_.sources.end(),
                     [](const auto& v) { return v.source.enabled && !v.disabled; })) {
        error = L"没有已启用的时间源。";
        return false;
    }
    manualPaused_ = false;
    state_.frozen = false;
    state_.syncing = true;
    ++syncEpoch_;
    state_.syncNote = L"自动轮询已开启；遵守每源最小间隔";
    SetEvent(wake_);
    return true;
}
void Engine::stopSync() {
    std::scoped_lock lock(mutex_);
    manualPaused_ = true;
    state_.syncing = false;
    ++syncEpoch_;
    state_.syncNote = L"手动暂停：点击开始后恢复自动同步";
}
void Engine::setSystemClockSync(bool enabled) {
    std::scoped_lock lock(mutex_);
    config_.autoSystemClock = enabled;
    state_.systemClockStatus = enabled ? L"等待自动校时" : L"系统校时未启用";
}
void Engine::setRefinement(bool enabled) {
    std::scoped_lock lock(mutex_);
    config_.preRefine = enabled;
    maybeFreezeLocked(qpc());
}
void Engine::resumeAfterJobLocked() {
    state_.frozen = false;
    state_.syncing = !manualPaused_;
    ++syncEpoch_;
    state_.syncNote = manualPaused_ ? L"保持手动暂停" : L"操作结束，已恢复自动同步";
    state_.refinementStarted = 0;
    state_.refinementSamples = 0;
    state_.refinementProgress = 0;
    state_.phase = state_.syncing ? SyncPhase::Tracking : SyncPhase::Paused;
    SetEvent(wake_);
}
bool Engine::configure(const Config& config, std::wstring& error) {
    std::scoped_lock lock(mutex_);
    if (state_.active) {
        error = L"请先取消待执行任务再修改配置。";
        return false;
    }
    ++syncEpoch_;
    config_ = config;
    state_.sources.clear();
    paths_.clear();
    state_.requestedFaults = config.faultBudget;
    state_.frequencyGroups = state_.toleratedFaults = 0;
    state_.frequencyMature = state_.faultToleranceReady = false;
    state_.refinementStarted = 0;
    state_.refinementSamples = 0;
    state_.refinementProgress = 0;
    state_.safeInterval = {};
    for (auto& s : config.sources) {
        SourceView v;
        v.source = s;
        state_.sources.push_back(v);
    }
    // Preserve rate-limit ledger across source edits and manual restart.
    state_.clock.calibrated = false;
    state_.clock.frequencyPpm = 0;
    state_.clock.residualPpm = config.driftAllowancePpm;
    state_.lastSuccessfulSyncQpc = 0;
    state_.lastSyncFailed = false;
    state_.primarySource.clear();
    state_.groups = state_.availableGroups = state_.consensusGroups = 0;
    maybeFreezeLocked(qpc());
    state_.syncNote = L"来源已更新，需要重新获得有效样本";
    SetEvent(wake_);
    return true;
}
void Engine::freezeLocked(Tick) {
    if (state_.frozen)
        return;
    state_.frozen = true;
    state_.phase = SyncPhase::Frozen;
    if (state_.refinementStarted)
        state_.refinementProgress = 1;
    state_.syncing = false;
    ++syncEpoch_;
    state_.frozenDeadline = state_.clock.deadline(job_.target, frequency);
    ++state_.preparationSerial;
    state_.syncNote = L"已冻结：停止 NTP 查询和时间模型更新";
    state_.taskStatus = L"正在准备执行 · 网络时间已冻结";
}
void Engine::maybeFreezeLocked(Tick now) {
    if (state_.active && !state_.frozen && freezeDue(state_.clock.utc(now, frequency), job_.target))
        freezeLocked(now);
    const auto next = syncPhase(state_.syncing, state_.frozen, state_.active, config_.preRefine,
                                state_.clock.utc(now, frequency), job_.target, config_.refinementLeadSeconds);
    if (next == SyncPhase::Refining) {
        if (!state_.refinementStarted) {
            state_.refinementStarted = now;
            state_.refinementSamples = 0;
        }
        const double remaining =
            (job_.target - state_.clock.utc(now, frequency)) / static_cast<double>(Second);
        state_.refinementProgress = std::clamp(
            (config_.refinementLeadSeconds - remaining) / (config_.refinementLeadSeconds - 60.0), 0.0, 1.0);
    } else if (next != SyncPhase::Frozen) {
        state_.refinementStarted = 0;
        state_.refinementProgress = 0;
        state_.refinementSamples = 0;
    }
    state_.phase = next == SyncPhase::Tracking && !state_.clock.calibrated ? SyncPhase::Discovering : next;
}
bool Engine::acquireEndpoint(const std::wstring& address, const Source& source, std::uint64_t epoch) {
    std::scoped_lock lock(mutex_);
    const auto now = qpc();
    maybeFreezeLocked(now);
    if (quitting_ || !state_.syncing || state_.frozen || epoch != syncEpoch_)
        return false;
    auto key = address + L":" + std::to_wstring(source.port);
    std::erase_if(nextEndpointQuery_, [now](const auto& entry) { return entry.second <= now; });
    if (nextEndpointQuery_[key] > now)
        return false;
    const int interval = state_.phase == SyncPhase::Refining
                             ? std::max(source.minPollSeconds, config_.refinementPollSeconds)
                             : source.minPollSeconds;
    nextEndpointQuery_[key] = now + static_cast<Tick>(interval) * frequency;
    return true;
}
bool Engine::cancelledQuery(std::uint64_t epoch) {
    if (quitting_)
        return true;
    std::scoped_lock lock(mutex_);
    maybeFreezeLocked(qpc());
    return !state_.syncing || epoch != syncEpoch_;
}
bool Engine::arm(const Job& job, std::wstring& error) {
    std::scoped_lock lock(mutex_);
    if (state_.active) {
        error = L"已有待执行任务。";
        return false;
    }
    if (!job.probe && !state_.clock.calibrated) {
        error = L"请先获得至少一个有效网络时间样本。可用“本机输入测试”验证本机调度。";
        return false;
    }
    if (!job.input.count || job.holdMs < 1 || job.holdMs > 10000 || job.repetitions < 1 ||
        job.repetitions > 1000 || job.intervalMs > 60000 ||
        (job.repetitions > 1 && job.intervalMs < job.holdMs + 5)) {
        error = L"按住需为 1–10000 ms；重复最多 1000 次；重复间隔至少为按住时间 + 5 ms。";
        return false;
    }
    Tick now = qpc();
    const auto utc = state_.clock.utc(now, frequency);
    if (job.target <= utc + 100 * Millisecond) {
        error = L"目标时间已经过去或不足 100 ms；不会自动顺延到下一小时。";
        return false;
    }
    if (job.target - utc > 24 * 3600LL * Second) {
        error = L"首版仅支持未来 24 小时内的任务。";
        return false;
    }
    if (!job.probe && state_.clock.uncertainty(now, frequency) > 1000) {
        error = L"时钟已过期或估计误差超过 1 秒，请重新同步。";
        return false;
    }
    job_ = job;
    state_.active = true;
    state_.frozen = false;
    state_.target = job.target;
    state_.actionSummary = job.input.description + L"  ·  " + std::to_wstring(job.repetitions) +
                           L" 次  ·  按住 " + std::to_wstring(job.holdMs) + L" ms";
    if (job.repetitions > 1)
        state_.actionSummary += L"  ·  间隔 " + std::to_wstring(job.intervalMs) + L" ms";
    state_.records.clear();
    state_.result.clear();
    state_.taskStatus = L"待执行 · 在目标前 60 秒冻结";
    ResetEvent(cancel_);
    maybeFreezeLocked(now);
    SetEvent(wake_);
    return true;
}
bool Engine::withdrawForEdit() {
    std::scoped_lock lock(mutex_);
    maybeFreezeLocked(qpc());
    if (state_.active && state_.frozen)
        return false;
    if (state_.active) {
        state_.active = false;
        state_.taskStatus = L"正在修改预约";
        state_.result.clear();
    }
    return true;
}
void Engine::cancel() {
    SetEvent(cancel_);
}
void Engine::sessionInterrupted() {
    cancel();
    std::scoped_lock lock(mutex_);
    state_.clock.calibrated = false;
    ++syncEpoch_;
    paths_.clear();
    state_.clock.frequencyPpm = 0;
    state_.clock.residualPpm = config_.driftAllowancePpm;
    state_.frequencyMature = state_.faultToleranceReady = false;
    state_.frequencyGroups = state_.groups = state_.availableGroups = state_.consensusGroups = 0;
    state_.toleratedFaults = 0;
    state_.safeInterval = {};
    state_.primarySource.clear();
    state_.lastSuccessfulSyncQpc = 0;
    for (auto& view : state_.sources) {
        view.last.reset();
        view.fit = {};
        view.selected = false;
        view.filteredSamples = 0;
    }
    state_.syncing = !manualPaused_;
    state_.syncNote = L"会话/电源状态变化：取消任务并重新校准";
}

void Engine::updateEstimateLocked(Tick now, Tick freshSince) {
    if (!state_.syncing || state_.frozen)
        return;
    std::map<std::wstring, std::size_t> representatives;
    for (std::size_t i = 0; i < state_.sources.size(); ++i) {
        auto& view = state_.sources[i];
        view.selected = false;
        view.filteredSamples = 0;
        if (!view.source.enabled || view.disabled || view.address.empty())
            continue;
        auto key = view.address + L":" + std::to_wstring(view.source.port);
        auto path = paths_.find(key);
        if (path == paths_.end() || !path->second.latest())
            continue;
        if (now - path->second.latest()->qpc > 180 * frequency)
            continue;
        auto found = representatives.find(key);
        if (found == representatives.end() || path->second.latest()->source == i)
            representatives[key] = i;
    }
    std::map<std::wstring, FrequencyFit> fits;
    for (const auto& [endpoint, index] : representatives) {
        auto& view = state_.sources[index];
        auto fit = paths_.at(endpoint).frequencyFit(frequency);
        view.fit = fit;
        if (!fit.valid)
            continue;
        auto group = operatorGroup(view.source);
        auto old = fits.find(group);
        if (old == fits.end() || fit.errorPpm < old->second.errorPpm)
            fits[group] = fit;
    }
    double ppm = state_.clock.frequencyPpm;
    double budget = std::max(config_.driftAllowancePpm, state_.clock.residualPpm);
    bool frequencyMature = false;
    if (!fits.empty()) {
        std::vector<Interval> intervals;
        std::vector<double> estimates;
        int mature = 0;
        for (const auto& [_, fit] : fits) {
            intervals.push_back({fit.ppm - fit.errorPpm, fit.ppm + fit.errorPpm});
            estimates.push_back(fit.ppm);
            if (fit.mature)
                ++mature;
        }
        auto result = selectIntervals(intervals, config_.faultBudget);
        if (result.valid) {
            ppm = std::clamp(median(estimates), result.envelope.lower, result.envelope.upper);
            const double fitBudget =
                std::max({2.0, ppm - result.envelope.lower, result.envelope.upper - ppm});
            frequencyMature = (config_.faultBudget == 0 || result.faultToleranceReady) &&
                              mature >= std::max(2, 2 * config_.faultBudget + 1);
            budget = frequencyMature ? fitBudget : std::max(config_.driftAllowancePpm, fitBudget);
        }
    }
    std::vector<Sample> candidates;
    for (const auto& [endpoint, index] : representatives) {
        auto& view = state_.sources[index];
        const bool refining = state_.phase == SyncPhase::Refining;
        auto filtered = paths_.at(endpoint).filtered(now, frequency, ppm, budget, refining ? 120 : 180,
                                                     refining ? state_.refinementStarted : 0);
        if (!filtered.valid) {
            view.status = filtered.reason;
            continue;
        }
        filtered.sample.source = index;
        filtered.sample.group = operatorGroup(view.source);
        filtered.sample.failurePenaltyMs = std::min(100.0, view.failures * 5.0);
        view.filteredSamples = static_cast<unsigned>(filtered.samples);
        view.basisAgeSeconds = filtered.basisAgeSeconds;
        view.qualityMs = sampleQuality(filtered.sample);
        candidates.push_back(filtered.sample);
    }
    auto estimate = combineSources(candidates, config_.faultBudget);
    if (!estimate.valid) {
        state_.lastSyncFailed = true;
        state_.syncNote = estimate.ambiguous ? L"存在多个可行时间区间，保留旧模型"
                                             : L"来源区间冲突或没有有效样本，保留旧模型";
        return;
    }
    bool freshSelected = false;
    for (const auto& sample : candidates)
        if (sample.qpc >= freshSince && std::find(estimate.usedSources.begin(), estimate.usedSources.end(),
                                                  sample.source) != estimate.usedSources.end())
            freshSelected = true;
    if (!freshSelected) {
        state_.lastSyncFailed = true;
        state_.syncNote = L"本轮未更新主用路径，保持原成功时间";
        return;
    }
    const Ns estimatedUtc =
        baseline.utc(now, frequency) + static_cast<Ns>(std::llround(estimate.offsetMs * Millisecond));
    ClockModel candidate{
        now, estimatedUtc, ppm, estimate.uncertaintyMs, budget, true, state_.clock.version + 1};
    const Tick commit = qpc();
    maybeFreezeLocked(commit);
    if (state_.frozen || !state_.syncing)
        return;
    if (state_.active && freezeDue(candidate.utc(commit, frequency), job_.target)) {
        freezeLocked(commit);
        return;
    }
    state_.clock = candidate;
    state_.frequencyMature = frequencyMature;
    state_.frequencyGroups = static_cast<int>(fits.size());
    state_.availableGroups = estimate.availableGroups;
    state_.requestedFaults = config_.faultBudget;
    state_.toleratedFaults = estimate.toleratedFaults;
    state_.faultToleranceReady = estimate.faultToleranceReady;
    state_.safeInterval = estimate.safeInterval;
    state_.lastSuccessfulSyncQpc = commit;
    state_.lastSyncFailed = false;
    state_.groups = estimate.groups;
    state_.consensusGroups = estimate.consensusGroups;
    state_.primarySource = state_.sources[estimate.referenceSource].source.host;
    for (auto i : estimate.usedSources)
        state_.sources[i].selected = true;
    state_.syncNote = L"主用 " + state_.primarySource + L"；" + std::to_wstring(estimate.consensusGroups) +
                      L"/" + std::to_wstring(estimate.availableGroups) + L" 组一致；";
    state_.syncNote += config_.faultBudget == 0 ? L"全组交集（F0）"
                       : estimate.faultToleranceReady
                           ? L"F" + std::to_wstring(estimate.toleratedFaults) + L" 区间假设"
                           : L"来源不足，降级参考";
    if (config_.autoSystemClock &&
        (!state_.active || job_.target - state_.clock.utc(commit, frequency) > 61 * Second)) {
        if (std::abs(static_cast<double>(systemUtc() - state_.clock.utc(qpc(), frequency))) < 2 * Millisecond)
            state_.systemClockStatus = L"本机已对齐";
        else if (commit - lastSystemWrite_ >= 10 * frequency) {
            auto result = clockSetter_(state_.clock);
            lastSystemWrite_ = commit;
            state_.systemClockStatus = result.success ? L"本机自动校时成功"
                                       : result.needsPrivilege
                                           ? L"本机校时需管理员权限"
                                           : L"本机校时失败 " + std::to_wstring(result.error);
        }
    }
}
void Engine::networkLoop() {
    while (!quitting_) {
        struct Request {
            std::size_t index;
            Source source;
            std::wstring preferred, avoided;
        };
        std::vector<Request> due;
        std::uint64_t epoch = 0;
        Tick roundStart = 0;
        bool storedNew = false, attempted = false;
        {
            std::scoped_lock lock(mutex_);
            const Tick now = qpc();
            maybeFreezeLocked(now);
            if (state_.syncing) {
                for (std::size_t i = 0; i < state_.sources.size() && due.size() < 6; ++i) {
                    auto& view = state_.sources[i];
                    auto key = sourceKey(view.source);
                    if (!view.source.enabled || view.disabled || nextQuery_[key] > now)
                        continue;
                    int interval = state_.phase == SyncPhase::Refining
                                       ? std::max(view.source.minPollSeconds, config_.refinementPollSeconds)
                                       : view.source.minPollSeconds;
                    nextQuery_[key] = now + static_cast<Tick>(interval) * frequency;
                    view.status = state_.phase == SyncPhase::Refining ? L"精校准采样中" : L"采样中";
                    due.push_back({i, view.source, view.consecutiveFailures < 2 ? view.address : L"",
                                   view.consecutiveFailures >= 2 ? view.address : L""});
                    ++state_.attempts;
                }
                epoch = syncEpoch_;
                roundStart = now;
                state_.busy = !due.empty();
            }
        }
        if (due.empty()) {
            WaitForSingleObject(wake_, 100);
            continue;
        }
        std::vector<std::future<NtpResult>> futures;
        for (const auto& request : due)
            futures.push_back(std::async(std::launch::async, [this, request, epoch] {
                return queryNtp(
                    request.source, request.index, baseline, [this, epoch] { return cancelledQuery(epoch); },
                    request.preferred, request.avoided,
                    [this, request, epoch](const std::wstring& address) {
                        return acquireEndpoint(address, request.source, epoch);
                    });
            }));
        std::vector<std::pair<Source, NtpResult>> results;
        for (std::size_t i = 0; i < futures.size(); ++i) {
            NtpResult result;
            try {
                result = futures[i].get();
            } catch (...) {
                result.status = L"采样工作线程异常";
            }
            const auto& request = due[i];
            {
                std::scoped_lock lock(mutex_);
                const Tick now = qpc();
                maybeFreezeLocked(now);
                if (result.backoffSeconds) {
                    auto until = now + static_cast<Tick>(result.backoffSeconds) * frequency;
                    nextQuery_[sourceKey(request.source)] =
                        std::max(nextQuery_[sourceKey(request.source)], until);
                    if (!result.address.empty()) {
                        auto key = result.address + L":" + std::to_wstring(request.source.port);
                        nextEndpointQuery_[key] = std::max(nextEndpointQuery_[key], until);
                    }
                }
                if (epoch != syncEpoch_ || !state_.syncing) {
                    if (result.ok)
                        ++state_.discardedAfterStop;
                    result.ok = false;
                    result.status = L"停止/冻结后不应用响应";
                    if (request.index < state_.sources.size() &&
                        sourceKey(state_.sources[request.index].source) == sourceKey(request.source))
                        state_.sources[request.index].status = result.status;
                } else if (request.index < state_.sources.size()) {
                    auto& view = state_.sources[request.index];
                    view.status = result.status;
                    view.disabled = result.disable;
                    if (!result.address.empty())
                        view.address = result.address;
                    attempted = attempted || !result.deferred;
                    if (!result.ok && !result.deferred) {
                        ++view.failures;
                        ++view.consecutiveFailures;
                    }
                    if (result.ok) {
                        view.consecutiveFailures = 0;
                        if (view.failures)
                            --view.failures;
                        auto endpoint = result.address + L":" + std::to_wstring(request.source.port);
                        if (!paths_.contains(endpoint) && paths_.size() >= 128) {
                            auto oldest = std::min_element(
                                paths_.begin(), paths_.end(), [](const auto& a, const auto& b) {
                                    const auto left = a.second.latest(), right = b.second.latest();
                                    return (left ? left->qpc : 0) < (right ? right->qpc : 0);
                                });
                            if (oldest != paths_.end())
                                paths_.erase(oldest);
                        }
                        result.sample.group = operatorGroup(request.source);
                        result.stored = paths_[endpoint].observe(result.sample, frequency);
                        if (result.stored) {
                            view.last = result.sample;
                            ++view.received;
                            ++state_.validSamples;
                            storedNew = true;
                            if (state_.phase == SyncPhase::Refining)
                                ++state_.refinementSamples;
                        }
                    }
                }
            }
            results.emplace_back(request.source, std::move(result));
        }
        Snapshot model;
        {
            std::scoped_lock lock(mutex_);
            maybeFreezeLocked(qpc());
            if (attempted)
                state_.lastAttemptQpc = qpc();
            if (epoch == syncEpoch_ && state_.syncing) {
                if (storedNew)
                    updateEstimateLocked(qpc(), roundStart);
                else if (attempted) {
                    state_.lastSyncFailed = true;
                    state_.syncNote = L"本轮对时失败，保持原成功时间";
                }
            }
            state_.busy = false;
            model = state_;
        }
        logSamples(results);
        if (attempted)
            logModel(model);
    }
}

void Engine::schedulerLoop() {
    HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while (!quitting_) {
        Job job;
        ClockModel model;
        Tick deadline = 0;
        {
            std::scoped_lock lock(mutex_);
            maybeFreezeLocked(qpc());
            if (state_.active && state_.frozen) {
                job = job_;
                model = state_.clock;
                deadline = state_.frozenDeadline;
            }
            if (state_.active && WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0) {
                state_.active = false;
                state_.taskStatus = L"已取消";
                state_.result = L"后续输入已停止";
                deadline = 0;
                resumeAfterJobLocked();
            }
        }
        if (!deadline) {
            if (quitting_)
                break;
            Sleep(10);
            continue;
        }
        std::vector<ExecutionRecord> records;
        records.reserve(job.repetitions);
        std::wstring status = L"完成";
        if (!timer)
            status = L"失败：系统不支持高分辨率等待定时器";
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        for (int i = 0; timer && i < job.repetitions; ++i) {
            const Tick target = deadline + static_cast<Tick>(std::llround(static_cast<long double>(i) *
                                                                          job.intervalMs * frequency / 1000));
            if (!preciseWait(target, timer, cancel_, frequency, std::max<Tick>(1, frequency / 1000))) {
                status = L"已取消或等待失败";
                break;
            }
            if (job.onlyWindow && GetForegroundWindow() != job.onlyWindow) {
                status = L"已取消：测试窗口失去前台焦点";
                break;
            }
            bool held = false;
            for (UINT k = 0; k < job.input.count; ++k)
                if (GetAsyncKeyState(job.input.checkKeys[k]) & 0x8000)
                    held = true;
            if (held) {
                status = L"已取消：用户正在按住目标按键/按钮";
                break;
            }
            if (WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0) {
                status = L"已取消";
                break;
            }
            ExecutionRecord rec;
            rec.index = i;
            rec.deadline = target;
            rec.before = qpc();
            if ((rec.before - target) * 1000.0 / frequency > job.lateToleranceMs) {
                records.push_back(rec);
                status = L"错过截止：未补发输入";
                break;
            }
            // No allocations, file I/O, network, or shared mutexes around submission.
            rec.inserted = SendInput(job.input.count, job.input.down.data(), sizeof(INPUT));
            rec.after = qpc();
            if (rec.inserted) {
                if (rec.inserted == job.input.count)
                    preciseWait(rec.after + static_cast<Tick>(job.holdMs) * frequency / 1000, timer, cancel_,
                                frequency, frequency / 1000);
                // Release only the prefix actually inserted, in reverse order.
                const UINT start = job.input.count - rec.inserted;
                rec.releasedCount = SendInput(rec.inserted, job.input.up.data() + start, sizeof(INPUT));
                rec.released = qpc();
            }
            records.push_back(rec);
            if (rec.inserted != job.input.count || rec.releasedCount != rec.inserted) {
                status = L"输入失败/释放不完整：检查目标权限（UIPI）或会话状态";
                break;
            }
            if (WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0) {
                status = L"已取消，已提交释放";
                break;
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        SetThreadExecutionState(ES_CONTINUOUS);
        logExecution(job, model, records, status);
        {
            std::scoped_lock lock(mutex_);
            state_.records = records;
            state_.active = false;
            resumeAfterJobLocked();
            state_.taskStatus = status;
            state_.result = status + (manualPaused_ ? L"；保持手动暂停同步" : L"；已自动恢复同步");
            if (status == L"完成")
                ++state_.completedCycles;
            if (status == L"完成" && job.hourly && !quitting_ &&
                WaitForSingleObject(cancel_, 0) != WAIT_OBJECT_0) {
                job_ = job;
                job_.target = nextHourlyTarget(job.target, state_.clock.utc(qpc(), frequency));
                state_.target = job_.target;
                state_.active = true;
                state_.taskStatus = L"等待下一次预约";
            }
        }
    }
    if (timer)
        CloseHandle(timer);
}
namespace {
std::string csv(const std::wstring& text) {
    std::string value = utf8(text), escaped = "\"";
    for (char c : value) {
        if (c == '\"')
            escaped += '\"';
        escaped += c;
    }
    return escaped + "\"";
}
const char* phaseName(SyncPhase phase) {
    switch (phase) {
    case SyncPhase::Discovering:
        return "discovering";
    case SyncPhase::Tracking:
        return "tracking";
    case SyncPhase::Refining:
        return "refining";
    case SyncPhase::Frozen:
        return "frozen";
    case SyncPhase::Paused:
        return "paused";
    }
    return "unknown";
}
} // namespace
void Engine::logSamples(const std::vector<std::pair<Source, NtpResult>>& results) {
    try {
        std::filesystem::create_directories(logDirectory_);
        auto path = logDirectory_ / L"ntp-samples-v2.csv";
        bool header = !std::filesystem::exists(path);
        std::ofstream out(path, std::ios::app);
        out << std::setprecision(15);
        if (header)
            out << "session,version,sample,qpc_frequency,baseline_qpc,baseline_utc_ns,source,group,ip,port,"
                   "sent,protocol_valid,stored,deferred,q1,q4,t1_ns,t2_ns,t3_ns,t4_ns,rtt_ms,offset_ms,root_"
                   "ms,status\n";
        for (const auto& [source, result] : results) {
            out << sessionId_ << ',' << CTIMER_VERSION_STRING << ',' << ++sampleSequence_ << ',' << frequency
                << ',' << baseline.anchorQpc << ',' << baseline.anchorUtc << ',' << csv(source.host) << ','
                << csv(operatorGroup(source)) << ',' << csv(result.address) << ',' << source.port << ','
                << result.sent << ',' << result.ok << ',' << result.stored << ',' << result.deferred << ','
                << result.sentQpc << ',' << result.receivedQpc << ',' << result.t1 << ',' << result.t2 << ','
                << result.t3 << ',' << result.t4 << ',' << result.sample.rttMs << ','
                << result.sample.offsetMs << ',' << result.sample.rootDistanceMs << ',' << csv(result.status)
                << '\n';
        }
    } catch (...) {
    }
}
void Engine::logModel(const Snapshot& snapshot) {
    try {
        std::filesystem::create_directories(logDirectory_);
        auto path = logDirectory_ / L"clock-models-v1.csv";
        bool header = !std::filesystem::exists(path);
        std::ofstream out(path, std::ios::app);
        out << std::setprecision(15);
        if (header)
            out << "session,version,observed_qpc,phase,model_version,anchor_qpc,anchor_utc_ns,rate_ppm,rate_"
                   "margin_ppm,frequency_mature,lower_ms,upper_ms,error_ms,groups,consensus_groups,requested_"
                   "faults,tolerated_faults,fault_ready,last_success_qpc,failed,refinement_samples,primary,"
                   "status\n";
        const auto& m = snapshot.clock;
        out << sessionId_ << ',' << CTIMER_VERSION_STRING << ',' << qpc() << ',' << phaseName(snapshot.phase)
            << ',' << m.version << ',' << m.anchorQpc << ',' << m.anchorUtc << ',' << m.frequencyPpm << ','
            << m.residualPpm << ',' << snapshot.frequencyMature << ',' << snapshot.safeInterval.lower << ','
            << snapshot.safeInterval.upper << ',' << m.errorMs << ',' << snapshot.groups << ','
            << snapshot.consensusGroups << ',' << snapshot.requestedFaults << ',' << snapshot.toleratedFaults
            << ',' << snapshot.faultToleranceReady << ',' << snapshot.lastSuccessfulSyncQpc << ','
            << snapshot.lastSyncFailed << ',' << snapshot.refinementSamples << ','
            << csv(snapshot.primarySource) << ',' << csv(snapshot.syncNote) << '\n';
    } catch (...) {
    }
}
void Engine::logExecution(const Job& job, const ClockModel& model,
                          const std::vector<ExecutionRecord>& records, const std::wstring& status) {
    try {
        std::filesystem::create_directories(logDirectory_);
        auto path = logDirectory_ / L"executions.csv";
        bool header = !std::filesystem::exists(path);
        std::ofstream out(path, std::ios::app);
        out << std::setprecision(12);
        if (header)
            out << "target_utc_ns,model_version,probe,index,deadline_qpc,before_qpc,after_qpc,release_qpc,"
                   "call_error_ms,call_duration_ms,inserted,released,status\n";
        for (const auto& r : records)
            out << job.target << ',' << model.version << ',' << job.probe << ',' << r.index << ','
                << r.deadline << ',' << r.before << ',' << r.after << ',' << r.released << ','
                << (r.before - r.deadline) * 1000.0 / frequency << ','
                << (r.after - r.before) * 1000.0 / frequency << ',' << r.inserted << ',' << r.releasedCount
                << ',' << utf8(status) << '\n';
    } catch (...) {
    }
}
} // namespace ct
