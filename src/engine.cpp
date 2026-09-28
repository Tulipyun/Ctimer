#include "engine.hpp"
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
void Engine::resumeAfterJobLocked() {
    state_.frozen = false;
    state_.syncing = !manualPaused_;
    ++syncEpoch_;
    state_.syncNote = manualPaused_ ? L"保持手动暂停" : L"操作结束，已恢复自动同步";
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
    phaseHistory_.clear();
    lastSelection_.clear();
    for (auto& s : config.sources) {
        SourceView v;
        v.source = s;
        state_.sources.push_back(v);
    }
    // Preserve rate-limit ledger across source edits and manual restart.
    state_.clock.calibrated = false;
    state_.clock.residualPpm = config.driftAllowancePpm;
    state_.lastSuccessfulSyncQpc = 0;
    state_.lastSyncFailed = false;
    state_.primarySource.clear();
    state_.groups = state_.availableGroups = 0;
    state_.syncNote = L"来源已更新，需要重新获得有效样本";
    SetEvent(wake_);
    return true;
}
void Engine::freezeLocked(Tick) {
    if (state_.frozen)
        return;
    state_.frozen = true;
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
    state_.syncing = !manualPaused_;
    state_.syncNote = L"会话/电源状态变化：取消任务并重新校准";
}

void Engine::updateEstimateLocked(Tick now, Tick freshSince) {
    if (!state_.syncing || state_.frozen)
        return;
    std::vector<Sample> candidates;
    std::set<std::wstring> seenAddresses;
    // Prefer low-delay valid sources, but always retain distinct configured groups.
    std::vector<size_t> order;
    for (size_t i = 0; i < state_.sources.size(); ++i) {
        state_.sources[i].selected = false;
        if (state_.sources[i].last)
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(),
              [&](auto a, auto b) { return state_.sources[a].last->rttMs < state_.sources[b].last->rttMs; });
    for (auto i : order) {
        auto& v = state_.sources[i];
        if (!v.source.enabled || v.disabled || !v.last)
            continue;
        auto s = *v.last;
        s.group = operatorGroup(v.source);
        std::vector<double> deviations;
        for (const auto& previous : v.history)
            deviations.push_back(previous.offsetMs);
        const double center = median(deviations);
        for (auto& offset : deviations)
            offset = std::abs(offset - center);
        s.jitterMs = median(deviations) * 1.4826;
        s.failurePenaltyMs = std::min(100.0, v.failures * 5.0);
        const auto age = static_cast<double>(now - s.qpc) / frequency;
        if (age > std::max(180.0, std::min(600.0, v.source.minPollSeconds * 2.5)))
            continue;
        if (!v.address.empty() && !seenAddresses.insert(v.address).second)
            continue;
        s.offsetMs += age * state_.clock.frequencyPpm / 1000.0;
        s.rootDistanceMs += age * config_.driftAllowancePpm / 1000.0;
        v.qualityMs = s.rttMs / 2 + s.rootDistanceMs + 2 * s.jitterMs + s.failurePenaltyMs;
        candidates.push_back(s);
    }
    auto estimate = combineSources(candidates);
    state_.availableGroups = estimate.availableGroups;
    if (!estimate.valid) {
        state_.lastSyncFailed = true;
        state_.syncNote = L"时间源分歧：未应用本轮估计，保持旧模型";
        return;
    }
    bool freshSelected = false;
    for (auto i : estimate.usedSources)
        if (state_.sources[i].last && state_.sources[i].last->qpc >= freshSince)
            freshSelected = true;
    if (!freshSelected) {
        state_.lastSyncFailed = true;
        state_.syncNote = L"主用来源未收到新样本，保持上次时间";
        return;
    }
    const Ns estimatedUtc =
        baseline.utc(now, frequency) + static_cast<Ns>(std::llround(estimate.offsetMs * Millisecond));
    if (state_.active && freezeDue(estimatedUtc, job_.target)) {
        freezeLocked(now);
        return;
    }
    double ppm = state_.clock.frequencyPpm;
    if (estimate.usedSources != lastSelection_) {
        phaseHistory_.clear();
        lastSelection_ = estimate.usedSources;
    }
    phaseHistory_.push_back({static_cast<double>(now - baseline.anchorQpc) / frequency, estimate.offsetMs});
    if (phaseHistory_.size() > 32)
        phaseHistory_.erase(phaseHistory_.begin());
    if (auto fit = fitFrequency(phaseHistory_))
        ppm = *fit;
    state_.clock = {now,
                    estimatedUtc,
                    ppm,
                    estimate.uncertaintyMs,
                    config_.driftAllowancePpm,
                    true,
                    state_.clock.version + 1};
    state_.lastSuccessfulSyncQpc = now;
    state_.lastSyncFailed = false;
    state_.groups = estimate.groups;
    state_.consensusGroups = estimate.consensusGroups;
    state_.primarySource = state_.sources[estimate.referenceSource].source.host;
    for (auto i : estimate.usedSources)
        state_.sources[i].selected = true;
    state_.syncNote = L"主用 " + state_.primarySource + L"；" + std::to_wstring(estimate.consensusGroups) +
                      L" 组来源一致，优选 " + std::to_wstring(estimate.groups) + L" 组";
    if (config_.autoSystemClock && (!state_.active || job_.target - estimatedUtc > 61 * Second)) {
        if (std::abs(static_cast<double>(systemUtc() - state_.clock.utc(qpc(), frequency))) < 2 * Millisecond)
            state_.systemClockStatus = L"本机已对齐";
        else if (now - lastSystemWrite_ >= 10 * frequency) {
            auto result = clockSetter_(state_.clock);
            lastSystemWrite_ = now;
            state_.systemClockStatus = result.success ? L"本机自动校时成功"
                                       : result.needsPrivilege
                                           ? L"本机校时需管理员权限"
                                           : L"本机校时失败 " + std::to_wstring(result.error);
        }
    }
}

void Engine::networkLoop() {
    while (!quitting_) {
        std::vector<std::pair<size_t, Source>> due;
        std::uint64_t epoch = 0;
        Tick roundStart{};
        bool acceptedNew = false;
        {
            std::scoped_lock lock(mutex_);
            const auto now = qpc();
            maybeFreezeLocked(now);
            if (state_.syncing) {
                for (size_t i = 0; i < state_.sources.size() && due.size() < 6; ++i) {
                    auto& v = state_.sources[i];
                    auto key = sourceKey(v.source);
                    if (!v.source.enabled || v.disabled || nextQuery_[key] > now)
                        continue;
                    nextQuery_[key] = now + static_cast<Tick>(v.source.minPollSeconds) * frequency;
                    v.status = L"采样中…";
                    due.emplace_back(i, v.source);
                    ++state_.attempts;
                }
                epoch = syncEpoch_;
                state_.busy = !due.empty();
                roundStart = now;
            }
        }
        if (due.empty()) {
            WaitForSingleObject(wake_, 100);
            continue;
        }
        std::vector<std::future<NtpResult>> futures;
        for (const auto& [i, s] : due)
            futures.push_back(std::async(std::launch::async, [this, i, s, epoch] {
                return queryNtp(s, i, baseline, [this, epoch] { return cancelledQuery(epoch); });
            }));
        std::vector<std::pair<Source, NtpResult>> log;
        for (size_t n = 0; n < futures.size(); ++n) {
            auto result = futures[n].get();
            const auto index = due[n].first;
            {
                std::scoped_lock lock(mutex_);
                auto now = qpc();
                maybeFreezeLocked(now);
                if (result.backoffSeconds)
                    nextQuery_[sourceKey(due[n].second)] = std::max(nextQuery_[sourceKey(due[n].second)],
                                                                    now + result.backoffSeconds * frequency);
                if (epoch != syncEpoch_ || !state_.syncing) {
                    if (result.ok)
                        ++state_.discardedAfterStop;
                    result.ok = false;
                    result.status = L"停止/冻结后不应用响应";
                    if (index < state_.sources.size() &&
                        sourceKey(state_.sources[index].source) == sourceKey(due[n].second))
                        state_.sources[index].status = L"已停止；未应用本轮响应";
                } else if (index < state_.sources.size()) {
                    auto& v = state_.sources[index];
                    v.status = result.status;
                    v.disabled = result.disable;
                    if (result.ok) {
                        if (!v.address.empty() && v.address != result.address) {
                            v.history.clear();
                            v.last.reset();
                        }
                        v.address = result.address;
                    }
                    if (!result.ok)
                        ++v.failures;
                    if (result.ok) {
                        if (v.failures)
                            --v.failures;
                        const auto currentMin = v.history.empty()
                                                    ? result.sample.rttMs
                                                    : std::min_element(v.history.begin(), v.history.end(),
                                                                       [](const auto& a, const auto& b) {
                                                                           return a.rttMs < b.rttMs;
                                                                       })
                                                          ->rttMs;
                        // Avoid persisting an old path's filter forever; age it at each source update.
                        while (!v.history.empty() && now - v.history.front().qpc > 600 * frequency)
                            v.history.pop_front();
                        if (v.history.size() >= 3 && result.sample.rttMs > currentMin * 3 + 5)
                            v.status = L"高延迟样本已剔除";
                        else {
                            v.last = result.sample;
                            ++v.received;
                            ++state_.validSamples;
                            acceptedNew = true;
                        }
                        v.history.push_back(result.sample);
                        if (v.history.size() > 8)
                            v.history.pop_front();
                    }
                }
            }
            log.emplace_back(due[n].second, std::move(result));
        }
        {
            std::scoped_lock lock(mutex_);
            maybeFreezeLocked(qpc());
            state_.lastAttemptQpc = qpc();
            if (epoch == syncEpoch_ && state_.syncing) {
                if (acceptedNew)
                    updateEstimateLocked(qpc(), roundStart);
                else {
                    state_.lastSyncFailed = true;
                    state_.syncNote = L"本轮对时失败，保持上次校准时间";
                }
            }
            state_.busy = false;
        }
        logSamples(log);
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
void Engine::logSamples(const std::vector<std::pair<Source, NtpResult>>& results) {
    try {
        std::filesystem::create_directories(logDirectory_);
        auto path = logDirectory_ / L"ntp-samples.csv";
        bool header = !std::filesystem::exists(path);
        std::ofstream out(path, std::ios::app);
        out << std::setprecision(12);
        if (header)
            out << "source,ip,accepted_protocol,t1_ns,t2_ns,t3_ns,t4_ns,offset_ms,rtt_ms,root_distance_ms,"
                   "status\n";
        for (const auto& [s, r] : results)
            out << utf8(s.host) << ',' << utf8(r.address) << ',' << r.ok << ',' << r.t1 << ',' << r.t2 << ','
                << r.t3 << ',' << r.t4 << ',' << r.sample.offsetMs << ',' << r.sample.rttMs << ','
                << r.sample.rootDistanceMs << ',' << utf8(r.status) << '\n';
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
