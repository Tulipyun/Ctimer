#include "platform.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <shlobj.h>

namespace ct {
Tick qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
Tick qpcFrequency() {
    static const Tick f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}
static constexpr std::uint64_t Epoch100ns = 116444736000000000ULL;
Ns systemUtc() {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    const auto v = (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    return static_cast<Ns>(v - Epoch100ns) * 100;
}
ClockModel initialClock() {
    ClockModel best;
    Tick span = INT64_MAX;
    for (int i = 0; i < 10; ++i) {
        Tick before = qpc();
        Ns utc = systemUtc();
        Tick after = qpc();
        if (after - before < span) {
            span = after - before;
            best.anchorQpc = before + span / 2;
            best.anchorUtc = utc;
        }
    }
    return best;
}
static SYSTEMTIME asSystemTime(Ns utc) {
    auto v = static_cast<std::uint64_t>(utc / 100) + Epoch100ns;
    FILETIME ft{static_cast<DWORD>(v), static_cast<DWORD>(v >> 32)};
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    return st;
}
SystemClockResult synchronizeSystemClock(const ClockModel& model) {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return {false, true, GetLastError()};
    TOKEN_PRIVILEGES requested{}, previous{};
    DWORD previousSize = sizeof(previous);
    requested.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEMTIME_NAME, &requested.Privileges[0].Luid)) {
        auto error = GetLastError();
        CloseHandle(token);
        return {false, true, error};
    }
    requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    bool adjusted =
        AdjustTokenPrivileges(token, FALSE, &requested, sizeof(previous), &previous, &previousSize) != FALSE;
    auto error = GetLastError();
    if (!adjusted || error == ERROR_NOT_ALL_ASSIGNED) {
        CloseHandle(token);
        return {false, true, error};
    }
    SYSTEMTIME utc = asSystemTime(model.utc(qpc(), qpcFrequency()));
    const bool ok = SetSystemTime(&utc) != FALSE;
    error = ok ? ERROR_SUCCESS : GetLastError();
    AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
    CloseHandle(token);
    return {ok, !ok && error == ERROR_PRIVILEGE_NOT_HELD, error};
}
bool hasSystemTimePrivilege() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD length{};
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &length);
    std::vector<unsigned char> buffer(length);
    LUID required{};
    bool found = false;
    if (length && LookupPrivilegeValueW(nullptr, SE_SYSTEMTIME_NAME, &required) &&
        GetTokenInformation(token, TokenPrivileges, buffer.data(), length, &length)) {
        auto privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(buffer.data());
        for (DWORD i = 0; i < privileges->PrivilegeCount; ++i)
            if (privileges->Privileges[i].Luid.LowPart == required.LowPart &&
                privileges->Privileges[i].Luid.HighPart == required.HighPart) {
                found = true;
                break;
            }
    }
    CloseHandle(token);
    return found;
}
std::wstring formatTime(Ns utc, bool date, int offsetMinutes) {
    const auto st = asSystemTime(utc + offsetMinutes * 60LL * Second);
    wchar_t b[80];
    if (date)
        swprintf_s(b, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour,
                   st.wMinute, st.wSecond, st.wMilliseconds);
    else
        swprintf_s(b, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}
std::optional<Ns> resolveTime(const std::wstring& text, Ns now, int offsetMinutes) {
    auto parsed = parseTime(text);
    if (!parsed)
        return {};
    auto st = asSystemTime(now + offsetMinutes * 60LL * Second);
    if (parsed->hour >= 0)
        st.wHour = static_cast<WORD>(parsed->hour);
    st.wMinute = static_cast<WORD>(parsed->minute);
    st.wSecond = static_cast<WORD>(parsed->second);
    st.wMilliseconds = static_cast<WORD>(parsed->millisecond);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
        return {};
    auto value = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<Ns>(value - Epoch100ns) * 100 - offsetMinutes * 60LL * Second;
}
std::wstring winError(DWORD code) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = buffer ? buffer : L"未知错误";
    if (buffer)
        LocalFree(buffer);
    return std::to_wstring(code) + L": " + text;
}
std::string utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}
std::wstring wide(const std::string& text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(),
                        n);
    return out;
}
std::filesystem::path executableDirectory() {
    wchar_t b[32768];
    auto n = GetModuleFileNameW(nullptr, b, 32768);
    return std::filesystem::path(std::wstring(b, n)).parent_path();
}

SavedWindowPlacement captureWindowPlacement(HWND window, bool compact, bool expanded) {
    SavedWindowPlacement result;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window, &placement))
        return result;
    RECT bounds{};
    if (!IsIconic(window) && !IsZoomed(window)) {
        if (!GetWindowRect(window, &bounds))
            return result;
    } else {
        // WINDOWPLACEMENT uses workspace coordinates; persist screen coordinates
        // so a taskbar on the top/left cannot cause creeping across restarts.
        bounds = placement.rcNormalPosition;
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info) &&
            !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
            OffsetRect(&bounds, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
    }
    result.valid = bounds.right > bounds.left && bounds.bottom > bounds.top;
    result.x = bounds.left;
    result.y = bounds.top;
    result.width = bounds.right - bounds.left;
    result.height = bounds.bottom - bounds.top;
    result.dpi = static_cast<int>(GetDpiForWindow(window));
    if (result.dpi <= 0)
        result.dpi = 96;
    result.compact = compact;
    result.expanded = expanded;
    result.maximized = IsZoomed(window) || (IsIconic(window) && (placement.flags & WPF_RESTORETOMAXIMIZED));
    return result;
}
RECT fitWindowToWorkArea(const SavedWindowPlacement& saved, const RECT& area, UINT targetDpi) {
    const auto workWidth = std::max<LONG>(1, area.right - area.left),
               workHeight = std::max<LONG>(1, area.bottom - area.top);
    const auto width = static_cast<LONG>(
        std::clamp<std::int64_t>(std::llround(static_cast<long double>(saved.width) *
                                              std::max(1u, targetDpi) / std::max(1, saved.dpi)),
                                 1, workWidth));
    const auto height = static_cast<LONG>(
        std::clamp<std::int64_t>(std::llround(static_cast<long double>(saved.height) *
                                              std::max(1u, targetDpi) / std::max(1, saved.dpi)),
                                 1, workHeight));
    const LONG x = std::clamp<LONG>(saved.x, area.left, area.right - width),
               y = std::clamp<LONG>(saved.y, area.top, area.bottom - height);
    return {x, y, x + width, y + height};
}

bool loadConfig(const std::filesystem::path& path, Config& config, std::wstring& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = L"无法读取配置文件。";
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    if (bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3);
    auto text = wide(bytes);
    if (text.empty()) {
        error = L"配置需为 UTF-8 文本。";
        return false;
    }
    std::wistringstream lines(text);
    std::wstring line, section, sources;
    Config next = config;
    auto trim = [](std::wstring s) {
        auto a = s.find_first_not_of(L" \t\r");
        return a == std::wstring::npos ? std::wstring{} : s.substr(a, s.find_last_not_of(L" \t\r") - a + 1);
    };
    try {
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty() || line[0] == L'#' || line[0] == L';')
                continue;
            if (line[0] == L'[') {
                section = line;
                continue;
            }
            if (section == L"[Sources]") {
                sources += line + L"\n";
                continue;
            }
            if (section != L"[General]")
                continue;
            auto equal = line.find(L'=');
            if (equal == std::wstring::npos)
                throw 1;
            auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
            if (key == L"Key") {
                next.key = value;
                continue;
            }
            size_t used = 0;
            int n = std::stoi(value, &used);
            if (used != value.size())
                throw 1;
            if (key == L"AutoSync")
                next.autoSync = n != 0;
            else if (key == L"AutoSchedule")
                next.autoSchedule = n != 0;
            else if (key == L"Sound")
                next.sound = n != 0;
            else if (key == L"Topmost")
                next.topmost = n != 0;
            else if (key == L"AutoSystemClock")
                next.autoSystemClock = n != 0;
            else if (key == L"AutoElevate")
                next.autoElevate = n != 0;
            else if (key == L"HasSchedule")
                next.hasSchedule = n != 0;
            else if (key == L"Minute")
                next.scheduleMinute = n;
            else if (key == L"Second")
                next.scheduleSecond = n;
            else if (key == L"Millisecond")
                next.scheduleMillisecond = n;
            else if (key == L"ActionKind")
                next.actionKind = n;
            else if (key == L"SourceCatalogVersion")
                next.sourceCatalogVersion = n;
            else if (key == L"HoldMs")
                next.holdMs = n;
            else if (key == L"IntervalMs")
                next.intervalMs = n;
            else if (key == L"Repetitions")
                next.repetitions = n;
            else if (key == L"UtcOffsetMinutes")
                next.utcOffsetMinutes = n;
            else if (key == L"LateToleranceMs")
                next.lateToleranceMs = n;
            else if (key == L"DriftAllowancePpm")
                next.driftAllowancePpm = n;
            else if (key == L"WindowSaved")
                next.window.valid = n != 0;
            else if (key == L"WindowX")
                next.window.x = n;
            else if (key == L"WindowY")
                next.window.y = n;
            else if (key == L"WindowWidth")
                next.window.width = n;
            else if (key == L"WindowHeight")
                next.window.height = n;
            else if (key == L"WindowDpi")
                next.window.dpi = n;
            else if (key == L"WindowCompact")
                next.window.compact = n != 0;
            else if (key == L"WindowExpanded")
                next.window.expanded = n != 0;
            else if (key == L"WindowMaximized")
                next.window.maximized = n != 0;
        }
        if (next.holdMs < 1 || next.holdMs > 10000 || next.intervalMs < 1 || next.intervalMs > 60000 ||
            next.repetitions < 1 || next.repetitions > 1000 || next.utcOffsetMinutes < -720 ||
            next.utcOffsetMinutes > 840 || next.lateToleranceMs < 1 || next.lateToleranceMs > 1000 ||
            next.driftAllowancePpm < 1 || next.driftAllowancePpm > 1000)
            throw 1;
        if (next.scheduleMinute < 0 || next.scheduleMinute > 59 || next.scheduleSecond < 0 ||
            next.scheduleSecond > 59 || next.scheduleMillisecond < 0 || next.scheduleMillisecond > 999 ||
            next.actionKind < 0 || next.actionKind > 3 || next.key.size() > 64 || !keyPlan(next.key))
            throw 1;
        if (next.window.valid &&
            (next.window.width < 120 || next.window.height < 80 || next.window.width > 100000 ||
             next.window.height > 100000 || next.window.dpi < 48 || next.window.dpi > 768 ||
             next.window.x < -1000000 || next.window.x > 1000000 || next.window.y < -1000000 ||
             next.window.y > 1000000))
            next.window.valid = false;
    } catch (...) {
        error = L"配置中数值或范围无效，旧配置保持不变。";
        return false;
    }
    auto parsed = parseSources(sources, error);
    if (!parsed)
        return false;
    next.sources = *parsed;
    config = next;
    return true;
}
bool saveConfig(const std::filesystem::path& path, const Config& c, std::wstring& error) {
    try {
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        const auto temp = std::filesystem::path(path.wstring() + L".tmp");
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << "# Ctimer UTF-8 configuration. Freeze before first down: fixed 60 seconds.\n[General]\n"
            << "AutoSync=" << c.autoSync << "\nAutoSchedule=" << c.autoSchedule << "\nSound=" << c.sound
            << "\nTopmost=" << c.topmost << "\nAutoSystemClock=" << c.autoSystemClock
            << "\nSourceCatalogVersion=" << c.sourceCatalogVersion << "\nHoldMs=" << c.holdMs
            << "\nIntervalMs=" << c.intervalMs << "\nRepetitions=" << c.repetitions
            << "\nUtcOffsetMinutes=" << c.utcOffsetMinutes << "\nLateToleranceMs=" << c.lateToleranceMs
            << "\nDriftAllowancePpm=" << static_cast<int>(c.driftAllowancePpm)
            << "\nAutoElevate=" << c.autoElevate << "\nHasSchedule=" << c.hasSchedule
            << "\nMinute=" << c.scheduleMinute << "\nSecond=" << c.scheduleSecond
            << "\nMillisecond=" << c.scheduleMillisecond << "\nActionKind=" << c.actionKind
            << "\nKey=" << utf8(c.key) << "\nWindowSaved=" << c.window.valid << "\nWindowX=" << c.window.x
            << "\nWindowY=" << c.window.y << "\nWindowWidth=" << c.window.width
            << "\nWindowHeight=" << c.window.height << "\nWindowDpi=" << c.window.dpi
            << "\nWindowCompact=" << c.window.compact << "\nWindowExpanded=" << c.window.expanded
            << "\nWindowMaximized=" << c.window.maximized
            << "\n\n[Sources]\n# host[:port] | operator_group | minimum_poll_seconds; prefix ! to disable\n"
            << utf8(serializeSources(c.sources));
        out.flush();
        if (!out) {
            error = L"写入配置失败。";
            return false;
        }
        out.close();
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = winError(GetLastError());
            return false;
        }
        return true;
    } catch (...) {
        error = L"无法保存配置，请检查目录写入权限。";
        return false;
    }
}

static int keyCode(std::wstring token) {
    if (token.size() == 1 &&
        ((token[0] >= L'A' && token[0] <= L'Z') || (token[0] >= L'0' && token[0] <= L'9')))
        return token[0];
    if (token.size() >= 2 && token[0] == L'F') {
        try {
            size_t n;
            int f = std::stoi(token.substr(1), &n);
            if (n == token.size() - 1 && f >= 1 && f <= 24)
                return VK_F1 + f - 1;
        } catch (...) {
        }
    }
    const std::pair<const wchar_t*, int> keys[] = {
        {L"CTRL", VK_CONTROL},  {L"ALT", VK_MENU},      {L"SHIFT", VK_SHIFT}, {L"ENTER", VK_RETURN},
        {L"SPACE", VK_SPACE},   {L"TAB", VK_TAB},       {L"ESC", VK_ESCAPE},  {L"BACKSPACE", VK_BACK},
        {L"DELETE", VK_DELETE}, {L"INSERT", VK_INSERT}, {L"HOME", VK_HOME},   {L"END", VK_END},
        {L"PAGEUP", VK_PRIOR},  {L"PAGEDOWN", VK_NEXT}, {L"LEFT", VK_LEFT},   {L"RIGHT", VK_RIGHT},
        {L"UP", VK_UP},         {L"DOWN", VK_DOWN}};
    for (auto [name, vk] : keys)
        if (token == name)
            return vk;
    return 0;
}
std::optional<InputPlan> keyPlan(const std::wstring& keys) {
    InputPlan p;
    p.description = keys;
    std::wistringstream in(keys);
    std::wstring part;
    while (std::getline(in, part, L'+')) {
        part.erase(std::remove_if(part.begin(), part.end(), [](wchar_t c) { return std::iswspace(c); }),
                   part.end());
        std::transform(part.begin(), part.end(), part.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        int vk = keyCode(part);
        if (!vk || p.count == 8)
            return {};
        for (UINT i = 0; i < p.count; ++i)
            if (p.checkKeys[i] == vk)
                return {};
        auto& d = p.down[p.count];
        d.type = INPUT_KEYBOARD;
        d.ki.wVk = static_cast<WORD>(vk);
        if (vk >= VK_PRIOR && vk <= VK_DELETE)
            d.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        d.ki.dwExtraInfo = 0x4354494D4552ULL;
        p.checkKeys[p.count] = vk;
        ++p.count;
    }
    if (p.count == 0 || (!keys.empty() && keys.back() == L'+'))
        return {};
    for (UINT i = 0; i < p.count; ++i) {
        p.up[i] = p.down[p.count - i - 1];
        p.up[i].ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    return p;
}
InputPlan mousePlan(int button) {
    InputPlan p;
    p.count = 1;
    p.down[0].type = INPUT_MOUSE;
    p.down[0].mi.dwExtraInfo = 0x4354494D4552ULL;
    p.up[0] = p.down[0];
    if (button == 1) {
        p.down[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        p.up[0].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        p.checkKeys[0] = VK_RBUTTON;
        p.description = L"鼠标右键";
    } else if (button == 2) {
        p.down[0].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
        p.up[0].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        p.checkKeys[0] = VK_MBUTTON;
        p.description = L"鼠标中键";
    } else {
        p.down[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        p.up[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        p.checkKeys[0] = VK_LBUTTON;
        p.description = L"鼠标左键";
    }
    return p;
}
bool preciseWait(Tick deadline, HANDLE timer, HANDLE cancel, Tick frequency, Tick spinTicks) {
    for (;;) {
        auto now = qpc();
        auto left = deadline - now;
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
            return false;
        if (left <= 0)
            return true;
        if (left > spinTicks) {
            LARGE_INTEGER due;
            due.QuadPart =
                -std::max<LONGLONG>(1, static_cast<LONGLONG>(static_cast<long double>(left - spinTicks) *
                                                             10'000'000 / frequency));
            if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
                return false;
            HANDLE handles[] = {cancel, timer};
            DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (result != WAIT_OBJECT_0 + 1)
                return false;
        } else {
            // Cancel is checked periodically without a kernel transition on every QPC read.
            unsigned spins = 0;
            while (qpc() < deadline) {
                YieldProcessor();
                if ((++spins & 255) == 0 && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
                    return false;
            }
            return WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0;
        }
    }
}
} // namespace ct
