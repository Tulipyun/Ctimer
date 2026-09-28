#pragma once
#include "core.hpp"
#include <winsock2.h>
#include <windows.h>
#include <array>
#include <filesystem>

namespace ct {
Tick qpc();
Tick qpcFrequency();
Ns systemUtc();
ClockModel initialClock();
std::wstring formatTime(Ns utc, bool date = false, int offsetMinutes = 480);
std::optional<Ns> resolveTime(const std::wstring& text, Ns now, int offsetMinutes = 480);
std::wstring winError(DWORD code);
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);
std::filesystem::path executableDirectory();

struct SavedWindowPlacement {
    bool valid{}, compact{}, expanded{}, maximized{};
    int x{}, y{}, width{}, height{}, dpi{96};
};
SavedWindowPlacement captureWindowPlacement(HWND window, bool compact, bool expanded);
RECT fitWindowToWorkArea(const SavedWindowPlacement& saved, const RECT& workArea, UINT targetDpi);

struct Config {
    SavedWindowPlacement window;
    std::vector<Source> sources{defaultSources()};
    bool autoSync{true};
    bool autoSchedule{true}, sound{true}, topmost{false};
    bool autoSystemClock{false}; // GUI opts in; library tests must never change the host clock.
    bool autoElevate{true}, hasSchedule{false};
    int scheduleMinute{}, scheduleSecond{}, scheduleMillisecond{}, actionKind{1};
    std::wstring key{L"F8"};
    int sourceCatalogVersion{};
    int holdMs{20}, intervalMs{100}, repetitions{1}, utcOffsetMinutes{480};
    int lateToleranceMs{10};
    double driftAllowancePpm{30};
};
bool loadConfig(const std::filesystem::path& path, Config& config, std::wstring& error);
bool saveConfig(const std::filesystem::path& path, const Config& config, std::wstring& error);
struct SystemClockResult {
    bool success{};
    bool needsPrivilege{};
    DWORD error{};
};
SystemClockResult synchronizeSystemClock(const ClockModel& model);
bool hasSystemTimePrivilege();

struct InputPlan {
    std::array<INPUT, 8> down{}, up{};
    UINT count{};
    std::array<int, 8> checkKeys{};
    std::wstring description;
};
std::optional<InputPlan> keyPlan(const std::wstring& keys);
InputPlan mousePlan(int button);
// Cancellable relative wait, then a bounded QPC spin. No absolute system-clock timer.
bool preciseWait(Tick deadline, HANDLE timer, HANDLE cancel, Tick frequency, Tick spinTicks);
} // namespace ct
