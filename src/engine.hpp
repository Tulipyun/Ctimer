#pragma once
#include "ntp.hpp"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace ct {
struct SourceView {
    Source source;
    std::wstring status{L"等待采样"}, address;
    std::optional<Sample> last;
    std::deque<Sample> history;
    unsigned received{};
    unsigned failures{};
    double qualityMs{};
    bool selected{}, disabled{};
};
struct Job {
    Ns target{};
    InputPlan input;
    int holdMs{20}, repetitions{1}, intervalMs{100}, lateToleranceMs{10};
    HWND onlyWindow{}; // Probe jobs are cancelled if our own window loses foreground.
    bool probe{};
    bool hourly{};
};
struct ExecutionRecord {
    int index{};
    Tick deadline{}, before{}, after{}, released{};
    UINT inserted{}, releasedCount{};
};
struct Snapshot {
    ClockModel clock;
    bool syncing{}, frozen{}, active{}, busy{};
    Ns target{};
    Tick frozenDeadline{};
    std::uint64_t preparationSerial{};
    std::wstring actionSummary, primarySource;
    std::wstring systemClockStatus{L"系统校时未启用"};
    unsigned completedCycles{};
    Tick lastSuccessfulSyncQpc{}, lastAttemptQpc{};
    bool lastSyncFailed{};
    int consensusGroups{};
    int groups{}, availableGroups{};
    unsigned attempts{}, validSamples{}, discardedAfterStop{};
    std::wstring taskStatus{L"尚未设置任务"}, result, syncNote{L"等待同步"};
    std::vector<SourceView> sources;
    std::vector<ExecutionRecord> records;
};
class Engine {
  public:
    Engine(Config config, std::filesystem::path logDirectory,
           std::function<SystemClockResult(const ClockModel&)> clockSetter = synchronizeSystemClock);
    ~Engine();
    Snapshot snapshot() const;
    bool startSync(std::wstring& error);
    void stopSync();
    bool configure(const Config& config, std::wstring& error);
    void setSystemClockSync(bool enabled);
    bool arm(const Job& job, std::wstring& error);
    bool withdrawForEdit();
    void cancel();
    void sessionInterrupted();
    void shutdown();
    const ClockModel baseline;
    const Tick frequency;

  private:
    mutable std::mutex mutex_;
    Config config_;
    Snapshot state_;
    Job job_;
    std::filesystem::path logDirectory_;
    std::atomic<bool> quitting_{};
    HANDLE wake_{}, cancel_{};
    std::jthread networkThread_, schedulerThread_;
    std::uint64_t syncEpoch_{};
    bool manualPaused_{};
    std::function<SystemClockResult(const ClockModel&)> clockSetter_;
    Tick lastSystemWrite_{};
    std::map<std::wstring, Tick> nextQuery_;
    std::vector<PhasePoint> phaseHistory_;
    std::vector<std::size_t> lastSelection_;
    void freezeLocked(Tick now);
    void maybeFreezeLocked(Tick now);
    void resumeAfterJobLocked();
    bool cancelledQuery(std::uint64_t epoch);
    void networkLoop();
    void schedulerLoop();
    void updateEstimateLocked(Tick now, Tick freshSince);
    void logSamples(const std::vector<std::pair<Source, NtpResult>>& results);
    void logExecution(const Job& job, const ClockModel& model, const std::vector<ExecutionRecord>& records,
                      const std::wstring& status);
};
} // namespace ct
