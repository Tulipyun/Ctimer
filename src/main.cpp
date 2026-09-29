#include "engine.hpp"
#include "app_info.hpp"
#include "clock_view.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <mmsystem.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

using namespace ct;
namespace {
enum Id {
    ClockLabel = 100,
    Quality,
    SyncNote,
    StartSync,
    StopSync,
    EditSources,
    OpenConfig,
    ReloadConfig,
    SourcesList,
    TargetEdit,
    FillFuture,
    ActionCombo,
    KeyEdit,
    HoldEdit,
    CountEdit,
    IntervalEdit,
    Arm,
    Cancel,
    Probe,
    Preview,
    TaskStatus,
    Result,
    AutoStart,
    AutoSchedule,
    Topmost,
    MiniMode,
    Details,
    Sound,
    ActionStatus,
    SecondEdit,
    MillisEdit,
    SignedOffset,
    StatusBar,
    AutoSystemClock,
    Elevate,
    PreRefine,
    SourceText = 200,
    SourceSave,
    SourceCancel
};
HINSTANCE instance;
HWND mainWindow{}, sourceWindow{};
HFONT normalFont{}, titleFont{}, errorFont{}, monoFont{}, bannerFont{};
HBRUSH backgroundBrush{}, readyBrush{}, scheduledBrush{};
HICON largeAppIcon{}, smallAppIcon{};
UINT dpi = 96;
int scrollPosition{};
std::unique_ptr<Engine> engine;
Config config;
Snapshot cached;
std::filesystem::path configPath, logPath;
bool smoke{}, smokeStarted{}, hotkeyRegistered{};
bool autoSmoke{}, autoSubmitted{}, initialized{}, programmaticEdit{}, mini{}, details{};
bool restoreSmoke{}, noAutoElevate{};
Tick saveSettingsAfter{};
AutoScheduleDraft draft;
std::wstring formNote;
std::uint64_t noticedSerial{};
int preparationNotices{};
double localMinusNtpMs{};
bool timeTouched{}, layoutBusy{};
int appExitCode{};
Tick smokeStart{}, observedDown{}, observedUp{};
int observedDownCount{}, observedUpCount{};
std::wstring lastSourcesFingerprint;
int px(int v) {
    return MulDiv(v, dpi, 96);
}
HWND control(int id) {
    return GetDlgItem(mainWindow, id);
}
std::wstring text(HWND window) {
    int n = GetWindowTextLengthW(window);
    std::wstring t(n + 1, L'\0');
    GetWindowTextW(window, t.data(), n + 1);
    t.resize(n);
    return t;
}
void setText(int id, const std::wstring& s) {
    auto h = control(id);
    if (h && text(h) != s) {
        SetWindowTextW(h, s.c_str());
        InvalidateRect(h, nullptr, TRUE);
    }
}
void alert(const std::wstring& s) {
    MessageBoxW(mainWindow, s.c_str(), app::ChineseName, MB_OK | MB_ICONINFORMATION);
}
void applyWindowIcons(HWND window) {
    auto large = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(100), IMAGE_ICON,
                                               GetSystemMetricsForDpi(SM_CXICON, dpi),
                                               GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
    auto small = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(100), IMAGE_ICON,
                                               GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                               GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0));
    if (large) {
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large));
        if (largeAppIcon)
            DestroyIcon(largeAppIcon);
        largeAppIcon = large;
    }
    if (small) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        if (smallAppIcon)
            DestroyIcon(smallAppIcon);
        smallAppIcon = small;
    }
}
void persistWindowPosition() {
    if (!initialized || smoke || !IsWindow(mainWindow))
        return;
    auto saved = captureWindowPlacement(mainWindow, mini, details);
    if (!saved.valid)
        return;
    config.window = saved;
    std::wstring error;
    if (!saveConfig(configPath, config, error))
        formNote = L"窗口位置保存失败：" + error;
}
void fonts() {
    for (auto f : {normalFont, titleFont, errorFont, monoFont, bannerFont})
        if (f)
            DeleteObject(f);
    normalFont =
        CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    titleFont =
        CreateFontW(-px(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    errorFont =
        CreateFontW(-px(19), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    monoFont =
        CreateFontW(-px(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    bannerFont =
        CreateFontW(-px(22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}
HWND add(HWND parent, const wchar_t* cls, const wchar_t* value, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, value, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
    return h;
}
void place(int id, int x, int y, int w, int h) {
    if (control(id))
        MoveWindow(control(id), px(x), px(y) - scrollPosition, px(w), px(h), !layoutBusy);
}
void layout() {
    if (layoutBusy)
        return;
    layoutBusy = true;
    RECT r;
    GetClientRect(mainWindow, &r);
    int w = std::max(mini ? 430 : 640, MulDiv(r.right, 96, dpi));
    const int height = mini ? 250 : (details ? 710 : 416);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    si.nMax = px(height) - 1;
    si.nPage = r.bottom;
    SetScrollInfo(mainWindow, SB_VERT, &si, TRUE);
    scrollPosition = std::clamp(scrollPosition, 0, std::max(0, px(height) - static_cast<int>(r.bottom)));
    for (int id : std::initializer_list<int>{304,
                                             305,
                                             306,
                                             307,
                                             308,
                                             309,
                                             TargetEdit,
                                             SecondEdit,
                                             MillisEdit,
                                             313,
                                             314,
                                             315,
                                             ActionCombo,
                                             KeyEdit,
                                             HoldEdit,
                                             CountEdit,
                                             IntervalEdit,
                                             AutoSchedule,
                                             AutoSystemClock,
                                             Arm,
                                             StartSync,
                                             StopSync,
                                             Details,
                                             301})
        ShowWindow(control(id), mini ? SW_HIDE : SW_SHOWNA);
    for (int id : {SourcesList, AutoStart, Sound, OpenConfig, ReloadConfig, EditSources, Elevate, PreRefine})
        ShowWindow(control(id), !mini && details ? SW_SHOWNA : SW_HIDE);
    for (int id :
         std::initializer_list<int>{303, 310, 311, 312, Preview, Result, SyncNote, 302, FillFuture, Probe})
        ShowWindow(control(id), SW_HIDE);
    place(300, 16, 9, 155, 34);
    place(301, 183, 22, 112, 20);
    place(Topmost, w - 216, 16, 90, 26);
    place(MiniMode, w - 115, 12, 100, 30);
    styleClockView(control(ClockLabel), dpi, mini);
    if (mini) {
        place(ClockLabel, 16, 52, 264, 62);
        place(SignedOffset, 287, 61, w - 303, 27);
        place(Quality, 287, 89, w - 303, 21);
        place(TaskStatus, 16, 122, w - 32, 36);
        place(ActionStatus, 16, 158, w - 32, 28);
        place(Cancel, 16, 199, 116, 30);
    } else {
        place(ClockLabel, 16, 51, 366, 76);
        place(SignedOffset, 394, 62, w - 410, 29);
        place(Quality, 394, 94, w - 410, 22);
        place(TaskStatus, 16, 130, w - 32, 35);
        place(ActionStatus, 16, 165, w - 32, 29);
        place(304, 16, 217, 60, 24);
        place(TargetEdit, 80, 212, 60, 33);
        place(313, 145, 217, 25, 24);
        place(SecondEdit, 175, 212, 60, 33);
        place(314, 240, 217, 25, 24);
        place(MillisEdit, 270, 212, 72, 33);
        place(315, 348, 217, 44, 24);
        place(AutoSchedule, 418, 215, 180, 26);
        place(305, 16, 266, 60, 24);
        place(ActionCombo, 80, 260, 150, 180);
        place(306, 248, 266, 43, 24);
        place(KeyEdit, 292, 260, 94, 33);
        place(307, 403, 266, 72, 24);
        place(HoldEdit, 477, 260, 65, 33);
        place(308, 16, 314, 66, 24);
        place(CountEdit, 85, 308, 65, 32);
        place(309, 175, 314, 125, 24);
        place(IntervalEdit, 304, 308, 76, 32);
        place(AutoSystemClock, 420, 311, 195, 26);
        place(Arm, 16, 358, 100, 32);
        place(Cancel, 127, 358, 103, 32);
        place(StartSync, 241, 358, 121, 32);
        place(StopSync, 373, 358, 104, 32);
        place(Details, 489, 358, 139, 32);
        place(AutoStart, 16, 430, 190, 28);
        place(Sound, 217, 430, 114, 28);
        place(EditSources, 346, 426, 124, 32);
        place(Elevate, 482, 426, 146, 32);
        place(OpenConfig, 16, 473, 139, 32);
        place(ReloadConfig, 166, 473, 148, 32);
        place(PreRefine, 350, 477, 240, 26);
        place(SourcesList, 16, 520, w - 32, 157);
    }
    SendMessageW(control(StatusBar), WM_SIZE, 0, 0);
    // Keep the top-level window visible while rearranging children; WM_SETREDRAW on
    // the root toggles WS_VISIBLE and can disturb activation during compact-mode changes.
    layoutBusy = false;
    RedrawWindow(mainWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void resizeMode() {
    scrollPosition = 0;
    RECT r{0, 0, px(mini ? 450 : 660), px(mini ? 250 : (details ? 710 : 416))};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW | WS_VSCROLL, FALSE, 0, dpi);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(mainWindow, nullptr, 0, 0, std::min(r.right - r.left, work.right - work.left),
                 std::min(r.bottom - r.top, work.bottom - work.top),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    setText(MiniMode, mini ? L"展开设置" : L"精简窗口");
    setText(Details, details ? L"收起设置" : L"设置 / 时间源");
    layout();
}
void applyTopmost() {
    SetWindowPos(mainWindow, config.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
std::wstring minuteTime(Ns t) {
    return formatTime(t, false, config.utcOffsetMinutes).substr(3);
}
std::optional<std::wstring> timeInput() {
    return normalizeTimeFields(text(control(TargetEdit)), text(control(SecondEdit)),
                               text(control(MillisEdit)));
}
void normalizeFields() {
    auto normalized = timeInput();
    if (!normalized)
        return;
    programmaticEdit = true;
    setText(TargetEdit, normalized->substr(0, 2));
    setText(SecondEdit, normalized->substr(3, 2));
    setText(MillisEdit, normalized->substr(6, 3));
    programmaticEdit = false;
}
void setTimeInput(const std::wstring& value) {
    programmaticEdit = true;
    setText(TargetEdit, value.substr(0, 2));
    setText(SecondEdit, value.substr(3, 2));
    setText(MillisEdit, value.substr(6, 3));
    programmaticEdit = false;
}
void changedForm() {
    if (!initialized || programmaticEdit || !engine->withdrawForEdit())
        return;
    timeTouched = true;
    saveSettingsAfter = qpc() + qpcFrequency();
    auto s = engine->snapshot();
    draft.changed(qpc(), qpcFrequency(), s.clock.utc(qpc(), qpcFrequency()));
    if (!config.autoSchedule)
        draft.cancel();
    formNote = config.autoSchedule ? L"填写完成 1 秒后自动预约，无需再点击按钮。"
                                   : L"自动预约已关闭，请点击“立即预约”。";
}
std::wstring savedTime() {
    wchar_t value[16];
    swprintf_s(value, L"%02d:%02d.%03d", config.scheduleMinute, config.scheduleSecond,
               config.scheduleMillisecond);
    return value;
}
bool readInt(int id, int& out);
bool persistForm() {
    if (!initialized || !timeTouched || (smoke && !autoSmoke))
        return false;
    auto value = timeInput();
    if (!value)
        return false;
    auto parsed = parseTime(*value);
    if (!parsed)
        return false;
    Config next = config;
    if (!readInt(HoldEdit, next.holdMs) || !readInt(CountEdit, next.repetitions) ||
        !readInt(IntervalEdit, next.intervalMs))
        return false;
    if (next.holdMs < 1 || next.holdMs > 10000 || next.repetitions < 1 || next.repetitions > 1000 ||
        next.intervalMs < 1 || next.intervalMs > 60000 ||
        (next.repetitions > 1 && next.intervalMs < next.holdMs + 5))
        return false;
    next.key = text(control(KeyEdit));
    if (!keyPlan(next.key))
        return false;
    next.hasSchedule = true;
    next.scheduleMinute = parsed->minute;
    next.scheduleSecond = parsed->second;
    next.scheduleMillisecond = parsed->millisecond;
    next.actionKind = static_cast<int>(SendMessageW(control(ActionCombo), CB_GETCURSEL, 0, 0));
    if (config.hasSchedule && next.scheduleMinute == config.scheduleMinute &&
        next.scheduleSecond == config.scheduleSecond &&
        next.scheduleMillisecond == config.scheduleMillisecond && next.actionKind == config.actionKind &&
        next.key == config.key && next.holdMs == config.holdMs && next.repetitions == config.repetitions &&
        next.intervalMs == config.intervalMs)
        return true;
    std::wstring error;
    if (!saveConfig(configPath, next, error)) {
        formNote = L"配置保存失败：" + error;
        return false;
    }
    config = next;
    return true;
}
void cancelTask() {
    draft.cancel();
    engine->cancel();
    formNote = L"已取消，不会自动重新预约；修改输入或点击按钮可再预约。";
}
bool readInt(int id, int& out) {
    auto s = text(control(id));
    if (s.empty() || s.size() > 6)
        return false;
    out = 0;
    for (auto c : s) {
        if (c < L'0' || c > L'9')
            return false;
        out = out * 10 + c - L'0';
    }
    return true;
}
std::wstring number(double value, int places = 3) {
    std::wostringstream o;
    o << std::fixed << std::setprecision(places) << value;
    return o.str();
}
void fillFuture() {
    auto s = engine->snapshot();
    auto now = s.clock.utc(qpc(), qpcFrequency());
    auto target = now + 90 * Second;
    auto offset = config.utcOffsetMinutes * 60LL * Second;
    if ((now + offset) / (3600 * Second) != (target + offset) / (3600 * Second)) {
        alert(L"本小时不足 90 秒，请直接填写分秒；不会跨到下一小时。");
        return;
    }
    setText(TargetEdit, minuteTime(target));
}
void preview() {
    if (!formNote.empty()) {
        setText(Preview, formNote);
        return;
    }
    setText(Preview, L"仅填写本小时 MM:SS.mmm，例如 59:59.123；毫秒为 000–999。");
}
void refreshSources() {
    std::wstring fingerprint;
    for (const auto& v : cached.sources)
        fingerprint += v.source.host + v.status + std::to_wstring(v.received) + (v.selected ? L"1" : L"0");
    if (fingerprint == lastSourcesFingerprint)
        return;
    lastSourcesFingerprint = fingerprint;
    HWND table = control(SourcesList);
    SendMessageW(table, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(table);
    std::vector<size_t> order;
    for (size_t i = 0; i < cached.sources.size(); ++i)
        order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) {
        const auto& x = cached.sources[a];
        const auto& y = cached.sources[b];
        const bool xp = x.selected && x.source.host == cached.primarySource,
                   yp = y.selected && y.source.host == cached.primarySource;
        if (xp != yp)
            return xp;
        if (x.selected != y.selected)
            return x.selected;
        if (bool(x.last) != bool(y.last))
            return bool(x.last);
        return x.qualityMs < y.qualityMs;
    });
    for (size_t row = 0; row < order.size(); ++row) {
        auto i = order[row];
        const auto& v = cached.sources[i];
        std::wstring domain = v.source.host + L"  [" + operatorGroup(v.source) + L"]";
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(row);
        item.pszText = domain.data();
        ListView_InsertItem(table, &item);
        std::wstring values[] = {
            v.last ? number(v.last->rttMs, 2) : L"—", v.last ? number(v.last->offsetMs, 2) : L"—",
            v.last ? number(v.qualityMs, 2) : L"—", std::to_wstring(v.received),
            !v.source.enabled
                ? L"已禁用"
                : (v.selected ? (v.source.host == cached.primarySource ? L"主用 · " : L"优选 · ")
                              : L"备选 · ") +
                      v.status};
        for (int col = 0; col < 5; ++col)
            ListView_SetItemText(table, static_cast<int>(row), col + 1, values[col].data());
    }
    SendMessageW(table, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(table, nullptr, TRUE);
}
void refresh() {
    cached = engine->snapshot();
    const auto q = qpc();
    const auto now = cached.clock.utc(q, qpcFrequency());
    auto before = qpc();
    auto local = systemUtc();
    auto after = qpc();
    localMinusNtpMs = (local - cached.clock.utc(before + (after - before) / 2, qpcFrequency())) / 1e6;
    setText(SignedOffset, cached.clock.calibrated ? (localMinusNtpMs >= 0 ? L"+" : L"−") +
                                                        number(std::abs(localMinusNtpMs), 3) + L" 毫秒"
                                                  : L"尚未校准");
    setText(Quality, cached.clock.calibrated
                         ? L"UTC ±" + number(cached.clock.uncertainty(q, qpcFrequency()), 1) + L" ms"
                         : L"UTC 待推定");
    const std::wstring syncTitle = cached.frozen    ? L"■ 已冻结"
                                   : cached.syncing ? L"● 自动同步中"
                                                    : L"○ 手动暂停";
    setText(SyncNote, mini ? syncTitle
                           : syncTitle + L"  " + cached.syncNote + L"\r\n有效样本 " +
                                 std::to_wstring(cached.validSamples) + L" / 查询 " +
                                 std::to_wstring(cached.attempts));
    const int minutes = config.utcOffsetMinutes;
    setText(302, L"UTC" + std::wstring(minutes >= 0 ? L"+" : L"−") + std::to_wstring(std::abs(minutes) / 60) +
                     L":" + (std::abs(minutes) % 60 < 10 ? L"0" : L"") +
                     std::to_wstring(std::abs(minutes) % 60) +
                     (cached.clock.calibrated
                          ? L"  ·  上次校准 " +
                                number((q - cached.clock.anchorQpc) * 1.0 / qpcFrequency(), 0) + L" 秒前"
                          : L"  ·  毫秒级时间输入"));
    if (cached.active) {
        setText(TaskStatus, cached.frozen
                                ? L"准备中  " + number(std::max(0.0, (cached.target - now) / 1e9), 3) + L" 秒"
                            : cached.phase == SyncPhase::Refining
                                ? L"精校准  " + number(cached.refinementProgress * 100, 0) + L"% · " +
                                      minuteTime(cached.target)
                                : L"下一次  " + minuteTime(cached.target));
        setText(ActionStatus, cached.actionSummary);
    } else if (draft.pending) {
        setText(TaskStatus, cached.clock.calibrated ? L"编辑中" : L"校时中");
        setText(ActionStatus, L"");
    } else {
        setText(TaskStatus, cached.result.empty() ? L"未预约" : cached.taskStatus);
        setText(ActionStatus, cached.result.empty() ? L"鼠标左键" : cached.actionSummary);
    }
    if (cached.preparationSerial != noticedSerial) {
        noticedSerial = cached.preparationSerial;
        if (cached.active) {
            ++preparationNotices;
            if (config.sound && cached.target - now > 500 * Millisecond) {
                if (!PlaySoundW(MAKEINTRESOURCEW(101), instance, SND_RESOURCE | SND_ASYNC | SND_NODEFAULT))
                    MessageBeep(MB_ICONEXCLAMATION);
            }
            FLASHWINFO flash{sizeof(flash), mainWindow, FLASHW_TRAY, 3, 0};
            FlashWindowEx(&flash);
        }
        InvalidateRect(control(TaskStatus), nullptr, TRUE);
        InvalidateRect(control(ActionStatus), nullptr, TRUE);
    }
    std::wstring detail = cached.result;
    if (!cached.records.empty()) {
        const auto& r = cached.records.front();
        detail +=
            L"；首个 down 调用误差 " + number((r.before - r.deadline) * 1000.0 / qpcFrequency()) + L" ms";
    }
    if (observedDownCount || observedUpCount)
        detail +=
            L"\r\n本机探针收到：down=" + std::to_wstring(observedDownCount) + L" / up=" +
            std::to_wstring(observedUpCount) +
            (observedUp > observedDown
                 ? L"，观测按住 " + number((observedUp - observedDown) * 1000.0 / qpcFrequency()) + L" ms"
                 : L"");
    setText(Result, detail.empty() ? L"等待任务。完成后显示调用误差；CSV 日志保存至配置目录的 logs 文件夹。"
                                   : detail);
    EnableWindow(control(StartSync), !(cached.active && cached.frozen));
    EnableWindow(control(StopSync), cached.syncing || cached.active);
    for (int id : {EditSources, ReloadConfig, Probe})
        EnableWindow(control(id), !cached.active);
    for (int id : {TargetEdit, SecondEdit, MillisEdit, ActionCombo, KeyEdit, HoldEdit, CountEdit,
                   IntervalEdit, AutoSchedule, Arm})
        EnableWindow(control(id), !(cached.active && cached.frozen));
    const bool keyboard = SendMessageW(control(ActionCombo), CB_GETCURSEL, 0, 0) == 0;
    EnableWindow(control(KeyEdit), keyboard && !(cached.active && cached.frozen));
    EnableWindow(control(Elevate), !cached.active);
    EnableWindow(control(Cancel), cached.active || draft.pending);
    preview();
    if (details)
        refreshSources();
    int parts[] = {px(mini ? 126 : 254), px(mini ? 290 : 456), -1};
    SendMessageW(control(StatusBar), SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    const auto since = cached.lastSuccessfulSyncQpc
                           ? std::max(0.0, (q - cached.lastSuccessfulSyncQpc) * 1.0 / qpcFrequency())
                           : 0;
    std::wstring primary;
    if (cached.frozen)
        primary = L"■ 冻结";
    else if (!cached.syncing)
        primary = L"Ⅱ 暂停";
    else if (cached.phase == SyncPhase::Refining)
        primary = cached.lastSyncFailed ? L"! 精校准未更新"
                  : mini                ? L"精校准 " + number(cached.refinementProgress * 100, 0) + L"%"
                                        : L"精校准 · " + std::to_wstring(cached.refinementSamples) + L" 样本";
    else if (cached.busy)
        primary = L"↻ 同步中";
    else if (cached.lastSyncFailed)
        primary = L"! 对时失败";
    else if (cached.clock.calibrated)
        primary = mini ? L"✓ 成功" : L"✓ 同步成功";
    else
        primary = L"未校准";
    if (cached.lastSuccessfulSyncQpc)
        primary += L" · " + number(since, 0) + L"秒前";
    std::wstring systemState = cached.systemClockStatus;
    if (mini && systemState.find(L"权限") != std::wstring::npos)
        systemState = L"本机需授权";
    std::wstring next = cached.active ? minuteTime(cached.target) : draft.pending ? L"编辑中" : L"未预约";
    SendMessageW(control(StatusBar), SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(primary.c_str()));
    SendMessageW(control(StatusBar), SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(systemState.c_str()));
    SendMessageW(control(StatusBar), SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(next.c_str()));
    auto syncTip = cached.syncNote + L"\r\n主用：" + cached.primarySource + L"\r\n成功校准后 " +
                   number(since, 1) + L" 秒";
    syncTip += L"\r\n频率：" + number(cached.clock.frequencyPpm, 3) + L" ppm；裕量 " +
               number(cached.clock.residualPpm, 2) + L" ppm" +
               (cached.frequencyMature ? L"（稳定样本）" : L"（仍在学习）");
    syncTip += L"\r\n请求容错 F" + std::to_wstring(cached.requestedFaults) + L"，当前 " +
               (cached.requestedFaults == 0  ? L"全组交集，无故障容忍"
                : cached.faultToleranceReady ? L"满足来源数量条件"
                                             : L"来源不足，降级参考");
    if (cached.phase == SyncPhase::Refining)
        syncTip += L"\r\n进度表示精校准窗口经过时间，不代表精度达标比例";
    SendMessageW(control(StatusBar), SB_SETTIPTEXTW, 0, reinterpret_cast<LPARAM>(syncTip.c_str()));
    SendMessageW(control(StatusBar), SB_SETTIPTEXTW, 2, reinterpret_cast<LPARAM>(formNote.c_str()));
}
void runJob(bool probe, bool automatic = false) {
    Job job;
    std::wstring error;
    auto s = engine->snapshot();
    auto now = s.clock.utc(qpc(), qpcFrequency());
    if (probe) {
        draft.cancel();
        job.target = now + 2 * Second;
        job.input = *keyPlan(L"F8");
        job.onlyWindow = mainWindow;
        job.probe = true;
        observedDown = observedUp = 0;
        observedDownCount = observedUpCount = 0;
        SetForegroundWindow(mainWindow);
        SetFocus(mainWindow);
    } else {
        if (!automatic && !engine->withdrawForEdit()) {
            alert(L"已经进入准备阶段，请先取消任务。");
            return;
        }
        auto fail = [&](const std::wstring& message) {
            formNote = message;
            draft.cancel();
            if (!automatic)
                alert(message);
        };
        auto value = timeInput();
        if (!value) {
            fail(L"分、秒需为 0–59；毫秒最多三位。");
            return;
        }
        auto t =
            resolveTime(*value, draft.hourReference ? draft.hourReference : now, config.utcOffsetMinutes);
        if (!t) {
            fail(L"时间无效。");
            return;
        }
        if (*t <= now + 100 * Millisecond)
            *t = nextHourlyTarget(*t, now);
        if (automatic && !s.clock.calibrated) {
            formNote = L"等待有效 NTP 校准，随后自动预约。";
            return;
        }
        job.target = *t;
        job.hourly = true;
        int action = static_cast<int>(SendMessageW(control(ActionCombo), CB_GETCURSEL, 0, 0));
        if (action == 0) {
            auto p = keyPlan(text(control(KeyEdit)));
            if (!p) {
                fail(L"按键示例：F8、A、ENTER、CTRL+F8。请勿输入不支持或重复的键。");
                return;
            }
            job.input = *p;
            job.input.description = L"键盘 " + job.input.description;
        } else
            job.input = mousePlan(action - 1);
        if (!readInt(HoldEdit, job.holdMs) || !readInt(CountEdit, job.repetitions) ||
            !readInt(IntervalEdit, job.intervalMs)) {
            fail(L"按住、次数和间隔必须为整数。");
            return;
        }
        job.lateToleranceMs = config.lateToleranceMs;
        if (autoSmoke)
            job.onlyWindow = mainWindow;
    }
    if (!engine->arm(job, error)) {
        formNote = error;
        draft.cancel();
        if (!automatic)
            alert(error);
        return;
    }
    draft.cancel();
    normalizeFields();
    formNote = L"已预约，每小时执行";
    if (automatic)
        autoSubmitted = true;
    if (!probe)
        persistForm();
    refresh();
}

LRESULT CALLBACK SourceProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        auto label = add(window, L"STATIC",
                         L"每行：域名 | 运营者分组 | 最小轮询秒数。添加/删除整行即可；前缀 ! 禁用。", 0, 400);
        MoveWindow(label, px(20), px(18), px(760), px(45), TRUE);
        auto edit = add(window, L"EDIT", serializeSources(config.sources).c_str(),
                        WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, SourceText,
                        WS_EX_CLIENTEDGE);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont), TRUE);
        MoveWindow(edit, px(20), px(72), px(760), px(295), TRUE);
        auto hint = add(window, L"STATIC",
                        L"公共源建议至少 64 秒。相同运营者请填写相同分组；仅输入域名也可以。\r\n可用 "
                        L"host:port 配置局域网服务。保存后需重新采样，旧时间不会冒充已校准。",
                        0, 401);
        MoveWindow(hint, px(20), px(380), px(760), px(55), TRUE);
        auto save = add(window, L"BUTTON", L"保存来源", BS_DEFPUSHBUTTON | WS_TABSTOP, SourceSave);
        MoveWindow(save, px(520), px(448), px(125), px(34), TRUE);
        auto cancel = add(window, L"BUTTON", L"取消", WS_TABSTOP, SourceCancel);
        MoveWindow(cancel, px(660), px(448), px(120), px(34), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == SourceCancel)
            DestroyWindow(window);
        if (LOWORD(wp) == SourceSave) {
            std::wstring error;
            auto parsed = parseSources(text(GetDlgItem(window, SourceText)), error);
            if (!parsed) {
                MessageBoxW(window, error.c_str(), L"来源格式", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            Config next = config;
            next.sources = *parsed;
            if (engine->snapshot().active) {
                alert(L"任务执行期间无法修改来源。");
                return 0;
            }
            if (!saveConfig(configPath, next, error) || !engine->configure(next, error)) {
                alert(error);
                return 0;
            }
            config = next;
            draft.cancel();
            lastSourcesFingerprint.clear();
            DestroyWindow(window);
            refresh();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        sourceWindow = nullptr;
        EnableWindow(mainWindow, TRUE);
        SetForegroundWindow(mainWindow);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), RGB(248, 250, 252));
        return reinterpret_cast<LRESULT>(backgroundBrush);
    }
    return DefWindowProcW(window, message, wp, lp);
}
void sourceEditor() {
    if (sourceWindow) {
        SetForegroundWindow(sourceWindow);
        return;
    }
    RECT r{0, 0, px(805), px(502)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    sourceWindow =
        CreateWindowExW(WS_EX_DLGMODALFRAME, L"CtimerSources", L"管理 NTP 时间源",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, mainWindow, nullptr, instance, nullptr);
    EnableWindow(mainWindow, FALSE);
}
void createControls() {
    add(mainWindow, L"STATIC", app::ChineseName, 0, 300);
    SendMessageW(control(300), WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
    add(mainWindow, L"STATIC", (std::wstring(L"v") + app::Version).c_str(), 0, 301);
    add(mainWindow, L"BUTTON", L"窗口置顶", BS_AUTOCHECKBOX | WS_TABSTOP, Topmost);
    SendMessageW(control(Topmost), BM_SETCHECK, config.topmost ? BST_CHECKED : BST_UNCHECKED, 0);
    add(mainWindow, L"BUTTON", L"精简窗口", WS_TABSTOP, MiniMode);
    add(mainWindow, L"BUTTON", L"设置 / 时间源", WS_TABSTOP, Details);
    add(mainWindow, L"BUTTON", L"自动预约", BS_AUTOCHECKBOX | WS_TABSTOP, AutoSchedule);
    SendMessageW(control(AutoSchedule), BM_SETCHECK, config.autoSchedule ? BST_CHECKED : BST_UNCHECKED, 0);
    add(mainWindow, L"BUTTON", L"准备时鸣音", BS_AUTOCHECKBOX | WS_TABSTOP, Sound);
    SendMessageW(control(Sound), BM_SETCHECK, config.sound ? BST_CHECKED : BST_UNCHECKED, 0);
    add(mainWindow, L"BUTTON", L"冻结前精校准", BS_AUTOCHECKBOX | WS_TABSTOP, PreRefine);
    SendMessageW(control(PreRefine), BM_SETCHECK, config.preRefine ? BST_CHECKED : BST_UNCHECKED, 0);
    add(mainWindow, L"BUTTON", L"自动校准系统时间", BS_AUTOCHECKBOX | WS_TABSTOP, AutoSystemClock);
    SendMessageW(control(AutoSystemClock), BM_SETCHECK, config.autoSystemClock ? BST_CHECKED : BST_UNCHECKED,
                 0);
    add(mainWindow, L"BUTTON", L"管理员运行", WS_TABSTOP, Elevate);
    add(mainWindow, ClockViewClass, L"00:00:00.000", 0, ClockLabel);
    int minutes = config.utcOffsetMinutes;
    std::wstring zone = L"UTC" + std::wstring(minutes >= 0 ? L"+" : L"−") +
                        std::to_wstring(std::abs(minutes) / 60) + L":" +
                        (std::abs(minutes) % 60 < 10 ? L"0" : L"") + std::to_wstring(std::abs(minutes) % 60) +
                        L"  ·  精度以 ms 管理";
    add(mainWindow, L"STATIC", zone.c_str(), 0, 302);
    add(mainWindow, L"STATIC", L"", 0, Quality);
    add(mainWindow, L"STATIC", L"", SS_NOTIFY, SignedOffset);
    SendMessageW(control(SignedOffset), WM_SETFONT, reinterpret_cast<WPARAM>(errorFont), TRUE);
    add(mainWindow, STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP | SBARS_TOOLTIPS, StatusBar);
    add(mainWindow, L"BUTTON", L"开始 / 继续同步", WS_TABSTOP, StartSync);
    add(mainWindow, L"BUTTON", L"停止同步", WS_TABSTOP, StopSync);
    add(mainWindow, L"BUTTON", L"管理时间源", WS_TABSTOP, EditSources);
    add(mainWindow, L"BUTTON", L"打开配置文件", WS_TABSTOP, OpenConfig);
    add(mainWindow, L"BUTTON", L"重新加载配置", WS_TABSTOP, ReloadConfig);
    add(mainWindow, L"BUTTON", L"启动时自动同步", WS_TABSTOP | BS_AUTOCHECKBOX, AutoStart);
    SendMessageW(control(AutoStart), BM_SETCHECK, config.autoSync ? BST_CHECKED : BST_UNCHECKED, 0);
    add(mainWindow, L"STATIC", L"", 0, SyncNote);
    auto list =
        add(mainWindow, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            SourcesList, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* headings[] = {L"时间源 / 分组", L"RTT ms", L"偏差 ms", L"评分", L"样本", L"状态"};
    const int widths[] = {202, 70, 76, 62, 43, 164};
    for (int i = 0; i < 6; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(headings[i]);
        col.cx = px(widths[i]);
        ListView_InsertColumn(list, i, &col);
    }
    add(mainWindow, L"STATIC", L"定时任务", 0, 303);
    add(mainWindow, L"STATIC", L"每小时", 0, 304);
    add(mainWindow, L"STATIC", L"操作", 0, 305);
    add(mainWindow, L"STATIC", L"按键", 0, 306);
    const auto initialTime = savedTime();
    add(mainWindow, L"EDIT", initialTime.substr(0, 2).c_str(), ES_NUMBER | ES_CENTER | WS_TABSTOP, TargetEdit,
        WS_EX_CLIENTEDGE);
    add(mainWindow, L"EDIT", initialTime.substr(3, 2).c_str(), ES_NUMBER | ES_CENTER | WS_TABSTOP, SecondEdit,
        WS_EX_CLIENTEDGE);
    add(mainWindow, L"EDIT", initialTime.substr(6, 3).c_str(), ES_NUMBER | ES_CENTER | WS_TABSTOP, MillisEdit,
        WS_EX_CLIENTEDGE);
    for (int id : {TargetEdit, SecondEdit, MillisEdit}) {
        SendMessageW(control(id), EM_SETLIMITTEXT, 6, 0);
        SendMessageW(control(id), WM_SETFONT, reinterpret_cast<WPARAM>(monoFont), TRUE);
    }
    add(mainWindow, L"STATIC", L"分", 0, 313);
    add(mainWindow, L"STATIC", L"秒", 0, 314);
    add(mainWindow, L"STATIC", L"毫秒", 0, 315);
    add(mainWindow, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, ActionCombo);
    for (auto s : {L"键盘按键", L"鼠标左键", L"鼠标右键", L"鼠标中键"})
        SendMessageW(control(ActionCombo), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(control(ActionCombo), CB_SETCURSEL, smoke ? 0 : config.actionKind, 0);
    add(mainWindow, L"EDIT", config.key.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, KeyEdit, WS_EX_CLIENTEDGE);
    add(mainWindow, L"STATIC", L"按住 ms", 0, 307);
    add(mainWindow, L"EDIT", std::to_wstring(config.holdMs).c_str(), ES_NUMBER | WS_TABSTOP, HoldEdit,
        WS_EX_CLIENTEDGE);
    add(mainWindow, L"STATIC", L"重复次数", 0, 308);
    add(mainWindow, L"EDIT", std::to_wstring(config.repetitions).c_str(), ES_NUMBER | WS_TABSTOP, CountEdit,
        WS_EX_CLIENTEDGE);
    add(mainWindow, L"STATIC", L"按下起点间隔 ms", 0, 309);
    add(mainWindow, L"EDIT", std::to_wstring(config.intervalMs).c_str(), ES_NUMBER | WS_TABSTOP, IntervalEdit,
        WS_EX_CLIENTEDGE);
    add(mainWindow, L"STATIC", L"点击使用执行时的光标位置", 0, 310);
    add(mainWindow, L"STATIC", L"", 0, Preview);
    add(mainWindow, L"BUTTON", L"预约", WS_TABSTOP, Arm);
    add(mainWindow, L"BUTTON", L"取消", WS_TABSTOP, Cancel);
    add(mainWindow, L"STATIC", L"全局停止：Ctrl + Alt + F10", 0, 311);
    add(mainWindow, L"STATIC", L"", 0, TaskStatus);
    add(mainWindow, L"STATIC", L"", 0, Result);
    add(mainWindow, L"STATIC", L"", 0, ActionStatus);
    SendMessageW(control(TaskStatus), WM_SETFONT, reinterpret_cast<WPARAM>(bannerFont), TRUE);
    add(mainWindow, L"STATIC", L"UTC 误差为估计，未独立验证。输入提交时间不等于目标软件处理时间。", 0, 312);
    auto tooltip =
        CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, mainWindow, nullptr, instance, nullptr);
    for (auto [id, tip] :
         {std::pair<int, const wchar_t*>{TargetEdit, L"分：0–59，留空为 00"},
          {SecondEdit, L"秒：0–59，留空为 00"},
          {MillisEdit, L"小数秒：1 → 100 毫秒，12 → 120 毫秒，001 → 1 毫秒；留空为 000"},
          {SignedOffset, L"本机时间 − NTP 推定 UTC。正值为本机偏快，负值为本机偏慢。"},
          {Quality,
           L"NTP 推定 UTC 的误差预算，包含网络延迟、上游误差与保持期间漂移；不是独立 UTC 实测误差。"},
          {Elevate, L"Windows 写入系统时间需要校时权限。管理员重启会出现系统授权提示。"}}) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = mainWindow;
        tool.uId = reinterpret_cast<UINT_PTR>(control(id));
        tool.lpszText = const_cast<LPWSTR>(tip);
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}
void restartElevated() {
    if (engine->snapshot().active) {
        formNote = L"请先取消预约再重启授权";
        return;
    }
    persistForm();
    persistWindowPosition();
    wchar_t executable[32768];
    GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring parameters = L"--config \"" + configPath.wstring() + L"\" --no-auto-elevate --wait-parent " +
                              std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_NOCLOSEPROCESS;
    request.hwnd = mainWindow;
    request.lpVerb = L"runas";
    request.lpFile = executable;
    request.lpParameters = parameters.c_str();
    request.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&request)) {
        formNote = GetLastError() == ERROR_CANCELLED ? L"未授予系统校时权限" : L"管理员重启失败";
        return;
    }
    if (request.hProcess)
        CloseHandle(request.hProcess);
    draft.cancel();
    DestroyWindow(mainWindow);
}
LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        mainWindow = window;
        dpi = GetDpiForWindow(window);
        fonts();
        createControls();
        applyWindowIcons(window);
        initialized = true;
        applyTopmost();
        setText(MiniMode, mini ? L"展开设置" : L"精简窗口");
        setText(Details, details ? L"收起设置" : L"设置 / 时间源");
        if (config.hasSchedule && config.autoSchedule) {
            timeTouched = true;
            changedForm();
            formNote = L"已恢复上次预约设置";
        }
        // F12 is reserved by Windows for debuggers; do not use it as a global hotkey.
        hotkeyRegistered = RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
        if (!hotkeyRegistered) {
            hotkeyRegistered = RegisterHotKey(window, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F11);
            if (hotkeyRegistered)
                setText(311, L"全局停止：Ctrl + Shift + F11");
        }
        if (!hotkeyRegistered)
            setText(311, L"全局停止注册失败，请使用取消按钮");
        WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
        SetTimer(window, 1, 33, nullptr);
        SetTimer(window, 2, 250, nullptr);
        refresh();
        return 0;
    case WM_SIZE:
        if (initialized)
            layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {px(mini ? 450 : 640), px(mini ? 282 : 370)};
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &si);
        int pos = scrollPosition;
        switch (LOWORD(wp)) {
        case SB_LINEUP:
            pos -= px(30);
            break;
        case SB_LINEDOWN:
            pos += px(30);
            break;
        case SB_PAGEUP:
            pos -= si.nPage;
            break;
        case SB_PAGEDOWN:
            pos += si.nPage;
            break;
        case SB_THUMBTRACK:
            pos = si.nTrackPos;
            break;
        }
        scrollPosition = std::clamp(pos, 0, std::max(0, si.nMax - static_cast<int>(si.nPage) + 1));
        SetScrollPos(window, SB_VERT, scrollPosition, TRUE);
        layout();
        return 0;
    }
    case WM_MOUSEWHEEL:
        SendMessageW(window, WM_VSCROLL, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        return 0;
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        fonts();
        applyWindowIcons(window);
        EnumChildWindows(
            window,
            [](HWND child, LPARAM) -> BOOL {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
                return TRUE;
            },
            0);
        SendMessageW(control(300), WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
        SendMessageW(control(TaskStatus), WM_SETFONT, reinterpret_cast<WPARAM>(bannerFont), TRUE);
        SendMessageW(control(SignedOffset), WM_SETFONT, reinterpret_cast<WPARAM>(errorFont), TRUE);
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_TIMER:
        if (wp == 1)
            updateClockView(control(ClockLabel), cached.clock.utc(qpc(), qpcFrequency()),
                            config.utcOffsetMinutes);
        else {
            refresh();
            if (saveSettingsAfter && qpc() >= saveSettingsAfter) {
                persistForm();
                saveSettingsAfter = 0;
            }
            if (config.autoSchedule && draft.ready(qpc()) && !cached.active && !sourceWindow)
                runJob(false, true);
            if (smoke && !smokeStarted) {
                smokeStarted = true;
                smokeStart = qpc();
                if (!autoSmoke)
                    runJob(true);
            }
            if (autoSmoke && smokeStarted && !autoSubmitted && !draft.pending && !timeTouched &&
                cached.clock.calibrated && GetForegroundWindow() == mainWindow) {
                auto now = cached.clock.utc(qpc(), qpcFrequency());
                auto target = now + 3 * Second;
                auto offset = config.utcOffsetMinutes * 60LL * Second;
                if ((now + offset) / (3600 * Second) == (target + offset) / (3600 * Second)) {
                    setTimeInput(minuteTime(target));
                    changedForm();
                    SetForegroundWindow(mainWindow);
                    SetFocus(mainWindow);
                }
            }
            if (restoreSmoke && cached.active) {
                std::filesystem::create_directories(logPath);
                std::ofstream report(logPath / L"restore-smoke.json");
                report << "{\"restored\":true,\"time\":\"" << utf8(timeInput().value_or(L""))
                       << "\",\"action_kind\":" << config.actionKind << ",\"automatic_armed\":true}";
                report.close();
                draft.cancel();
                engine->cancel();
                smoke = false;
                DestroyWindow(window);
                return 0;
            }
            if (smokeStarted && ((!cached.records.empty() && observedUpCount > 0) ||
                                 qpc() - smokeStart > qpcFrequency() * (autoSmoke ? 45 : 12))) {
                bool ok = observedDownCount == 1 && observedUpCount == 1 && !cached.records.empty() &&
                          cached.records[0].inserted == 1 && cached.records[0].releasedCount == 1 &&
                          cached.records[0].before >= cached.records[0].deadline && preparationNotices == 1 &&
                          (!autoSmoke || autoSubmitted);
                std::filesystem::create_directories(logPath);
                std::ofstream report(logPath / L"input-smoke.json");
                report << "{\"passed\":" << (ok ? "true" : "false")
                       << ",\"automatic\":" << (autoSmoke ? "true" : "false") << ",\"next_hour_scheduled\":"
                       << (cached.active &&
                                   cached.target - cached.clock.utc(qpc(), qpcFrequency()) > 3500 * Second
                               ? "true"
                               : "false")
                       << ",\"preparation_notices\":" << preparationNotices
                       << ",\"sync_resumed\":" << (cached.syncing ? "true" : "false") << ",\"status\":\""
                       << utf8(cached.taskStatus) << "\",\"down\":" << observedDownCount
                       << ",\"up\":" << observedUpCount
                       << ",\"observed_hold_ms\":" << (observedUp - observedDown) * 1000.0 / qpcFrequency()
                       << "}";
                report.close();
                smokeStarted = false;
                smoke = false;
                appExitCode = ok ? 0 : 1;
                DestroyWindow(window);
            }
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        std::wstring error;
        const bool timeField = id == TargetEdit || id == SecondEdit || id == MillisEdit;
        if (timeField && HIWORD(wp) == EN_SETFOCUS) {
            SendMessageW(control(id), EM_SETSEL, 0, -1);
            return 0;
        }
        if (timeField && HIWORD(wp) == EN_KILLFOCUS) {
            normalizeFields();
            return 0;
        }
        if ((timeField || id == KeyEdit || id == HoldEdit || id == CountEdit || id == IntervalEdit) &&
            HIWORD(wp) == EN_CHANGE) {
            changedForm();
            return 0;
        }
        if (id == ActionCombo && HIWORD(wp) == CBN_SELCHANGE) {
            changedForm();
            return 0;
        }
        if (HIWORD(wp) != BN_CLICKED)
            return 0;
        switch (id) {
        case StartSync:
            if (!engine->startSync(error))
                alert(error);
            break;
        case StopSync:
            engine->stopSync();
            break;
        case EditSources:
            sourceEditor();
            break;
        case OpenConfig:
            ShellExecuteW(window, L"open", L"notepad.exe", (L"\"" + configPath.wstring() + L"\"").c_str(),
                          nullptr, SW_SHOWNORMAL);
            break;
        case ReloadConfig: {
            Config next = config;
            if (!loadConfig(configPath, next, error) || !engine->configure(next, error))
                alert(error);
            else {
                config = next;
                draft.cancel();
                programmaticEdit = true;
                setTimeInput(savedTime());
                timeTouched = config.hasSchedule;
                SendMessageW(control(ActionCombo), CB_SETCURSEL, config.actionKind, 0);
                setText(KeyEdit, config.key);
                setText(HoldEdit, std::to_wstring(config.holdMs));
                setText(CountEdit, std::to_wstring(config.repetitions));
                setText(IntervalEdit, std::to_wstring(config.intervalMs));
                programmaticEdit = false;
                for (auto [check, value] :
                     {std::pair{AutoStart, config.autoSync}, std::pair{AutoSchedule, config.autoSchedule},
                      std::pair{Sound, config.sound}, std::pair{Topmost, config.topmost},
                      std::pair{AutoSystemClock, config.autoSystemClock},
                      std::pair{PreRefine, config.preRefine}})
                    SendMessageW(control(check), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
                applyTopmost();
                lastSourcesFingerprint.clear();
                if (config.hasSchedule && config.autoSchedule)
                    changedForm();
            }
            break;
        }
        case AutoStart:
            config.autoSync = SendMessageW(control(AutoStart), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!saveConfig(configPath, config, error))
                alert(error);
            break;
        case AutoSchedule:
            config.autoSchedule = SendMessageW(control(AutoSchedule), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (config.autoSchedule)
                changedForm();
            else {
                draft.cancel();
                engine->withdrawForEdit();
                formNote = L"自动预约已关闭，请点击立即预约。";
            }
            if (!saveConfig(configPath, config, error))
                alert(error);
            break;
        case Topmost:
            config.topmost = SendMessageW(control(Topmost), BM_GETCHECK, 0, 0) == BST_CHECKED;
            applyTopmost();
            if (!saveConfig(configPath, config, error))
                alert(error);
            break;
        case Sound:
            config.sound = SendMessageW(control(Sound), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!saveConfig(configPath, config, error))
                alert(error);
            break;
        case AutoSystemClock:
            config.autoSystemClock = SendMessageW(control(AutoSystemClock), BM_GETCHECK, 0, 0) == BST_CHECKED;
            engine->setSystemClockSync(config.autoSystemClock);
            if (!saveConfig(configPath, config, error))
                alert(error);
            break;
        case PreRefine:
            config.preRefine = SendMessageW(control(PreRefine), BM_GETCHECK, 0, 0) == BST_CHECKED;
            engine->setRefinement(config.preRefine);
            if (!saveConfig(configPath, config, error))
                alert(error);
            break;
        case Elevate:
            restartElevated();
            return 0;
        case MiniMode:
            mini = !mini;
            resizeMode();
            break;
        case Details:
            details = !details;
            resizeMode();
            lastSourcesFingerprint.clear();
            break;
        case FillFuture:
            fillFuture();
            break;
        case Arm:
            runJob(false);
            break;
        case Cancel:
            cancelTask();
            break;
        case Probe:
            runJob(true);
            break;
        }
        refresh();
        return 0;
    }
    case WM_HOTKEY:
        cancelTask();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND || wp == PBT_APMRESUMEAUTOMATIC) {
            draft.cancel();
            engine->sessionInterrupted();
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_LOGOFF || wp == WTS_CONSOLE_DISCONNECT ||
            wp == WTS_REMOTE_DISCONNECT) {
            draft.cancel();
            engine->sessionInterrupted();
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        auto label = reinterpret_cast<HWND>(lp);
        if (label == control(SignedOffset)) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(248, 250, 252));
            SetTextColor(dc, !cached.clock.calibrated ? RGB(100, 110, 120)
                             : localMinusNtpMs >= 0   ? RGB(193, 42, 35)
                                                      : RGB(20, 126, 73));
            return reinterpret_cast<LRESULT>(backgroundBrush);
        }
        if (label == control(TaskStatus) || label == control(ActionStatus)) {
            bool ready = cached.active && cached.frozen;
            SetBkColor(dc, ready ? RGB(255, 236, 194) : RGB(231, 242, 249));
            SetTextColor(dc, ready ? RGB(115, 54, 0) : RGB(17, 73, 102));
            return reinterpret_cast<LRESULT>(ready ? readyBrush : scheduledBrush);
        }
        SetBkColor(dc, RGB(248, 250, 252));
        SetTextColor(dc, RGB(40, 53, 67));
        return reinterpret_cast<LRESULT>(backgroundBrush);
    }
    case WM_EXITSIZEMOVE:
        persistWindowPosition();
        return 0;
    case WM_QUERYENDSESSION:
        persistForm();
        persistWindowPosition();
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            draft.cancel();
            engine->cancel();
            persistForm();
            persistWindowPosition();
        }
        return 0;
    case WM_CLOSE:
        draft.cancel();
        engine->cancel();
        persistForm();
        persistWindowPosition();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (hotkeyRegistered)
            UnregisterHotKey(window, 1);
        WTSUnRegisterSessionNotification(window);
        PostQuitMessage(appExitCode);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR, int show) {
    instance = h;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    DWORD waitParent = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    configPath = executableDirectory() / L"Ctimer.ini";
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--config" && i + 1 < argc)
            configPath = std::filesystem::absolute(argv[++i]);
        else if (std::wstring(argv[i]) == L"--input-smoke-test")
            smoke = true;
        else if (std::wstring(argv[i]) == L"--auto-smoke-test") {
            smoke = true;
            autoSmoke = true;
        } else if (std::wstring(argv[i]) == L"--restore-smoke-test") {
            smoke = true;
            autoSmoke = true;
            restoreSmoke = true;
        } else if (std::wstring(argv[i]) == L"--no-auto-elevate")
            noAutoElevate = true;
        else if (std::wstring(argv[i]) == L"--wait-parent" && i + 1 < argc)
            waitParent = wcstoul(argv[++i], nullptr, 10);
    }
    LocalFree(argv);
    std::wstring error;
    if (waitParent && waitParent != GetCurrentProcessId()) {
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, waitParent);
        if (parent) {
            WaitForSingleObject(parent, 5000);
            CloseHandle(parent);
        }
    }
    const auto mutexName =
        L"Local\\Ctimer-" + std::to_wstring(std::hash<std::wstring>{}(configPath.wstring()));
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"CtimerMain", nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(instanceMutex);
        return 0;
    }
    if (std::filesystem::exists(configPath)) {
        if (!loadConfig(configPath, config, error)) {
            MessageBoxW(nullptr, error.c_str(), L"定时点击器 · 配置错误", MB_OK | MB_ICONERROR);
            return 2;
        }
    } else if (!saveConfig(configPath, config, error)) {
        MessageBoxW(nullptr, (L"无法创建配置：" + error).c_str(), app::ChineseName, MB_OK | MB_ICONERROR);
        return 2;
    }
    if (!smoke && config.sourceCatalogVersion < 4) {
        std::error_code backupError;
        std::filesystem::copy_file(configPath,
                                   std::filesystem::path(configPath.wstring() + L".before-v0.6.bak"),
                                   std::filesystem::copy_options::skip_existing, backupError);
        if (config.sourceCatalogVersion < 3) {
            addRegionalSources(config.sources);
            for (auto& source : config.sources) {
                auto group = operatorGroup(source);
                if (group == L"aliyun" || group == L"tencent" || group == L"cloudflare" || group == L"nict" ||
                    group == L"nist")
                    source.minPollSeconds = 10;
            }
            config.autoSync = true;
            config.autoSystemClock = true;
        }
        addDiverseSources(config.sources);
        config.sourceCatalogVersion = 4;
        if (!saveConfig(configPath, config, error)) {
            MessageBoxW(nullptr, error.c_str(), L"定时点击器 · 配置保存失败", MB_OK);
            return 2;
        }
    }
    if (smoke) {
        config.autoSync = autoSmoke;
        config.autoSchedule = autoSmoke;
        config.sound = false;
        config.topmost = false;
        config.autoSystemClock = false;
    }
    if (!smoke && config.autoSystemClock && config.autoElevate && !noAutoElevate &&
        !hasSystemTimePrivilege()) {
        wchar_t executable[32768];
        GetModuleFileNameW(nullptr, executable, 32768);
        auto parameters = L"--config \"" + configPath.wstring() + L"\" --no-auto-elevate --wait-parent " +
                          std::to_wstring(GetCurrentProcessId());
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.fMask = SEE_MASK_NOCLOSEPROCESS;
        request.lpVerb = L"runas";
        request.lpFile = executable;
        request.lpParameters = parameters.c_str();
        request.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&request)) {
            if (request.hProcess)
                CloseHandle(request.hProcess);
            if (instanceMutex)
                CloseHandle(instanceMutex);
            return 0;
        }
    }
    logPath = configPath.parent_path() / L"logs";
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return 3;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    if (!registerClockView(h))
        return 4;
    backgroundBrush = CreateSolidBrush(RGB(248, 250, 252));
    readyBrush = CreateSolidBrush(RGB(255, 236, 194));
    scheduledBrush = CreateSolidBrush(RGB(231, 242, 249));
    WNDCLASSW wc{};
    wc.hInstance = h;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(h, MAKEINTRESOURCEW(100));
    wc.hbrBackground = backgroundBrush;
    wc.lpszClassName = L"CtimerMain";
    wc.lpfnWndProc = MainProc;
    RegisterClassW(&wc);
    wc.lpszClassName = L"CtimerSources";
    wc.lpfnWndProc = SourceProc;
    RegisterClassW(&wc);
    engine = std::make_unique<Engine>(config, logPath);
    cached = engine->snapshot();
    dpi = GetDpiForSystem();
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const auto savedWindow = config.window;
    if (savedWindow.valid) {
        mini = savedWindow.compact;
        details = savedWindow.expanded;
    }
    RECT rect{0, 0, px(mini ? 450 : 660), px(mini ? 250 : (details ? 710 : 416))};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN, FALSE, 0, dpi);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT,
        width = std::min(rect.right - rect.left, work.right - work.left),
        height = std::min(rect.bottom - rect.top, work.bottom - work.top);
    if (savedWindow.valid) {
        RECT previous{savedWindow.x, savedWindow.y, savedWindow.x + savedWindow.width,
                      savedWindow.y + savedWindow.height};
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromRect(&previous, MONITOR_DEFAULTTONEAREST), &monitor))
            work = monitor.rcWork;
        rect = fitWindowToWorkArea(savedWindow, work, dpi);
        x = rect.left;
        y = rect.top;
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
    }
    HWND window = CreateWindowExW(0, L"CtimerMain", app::WindowTitle,
                                  WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN, x, y, width, height,
                                  nullptr, nullptr, h, nullptr);
    if (!window)
        return 4;
    if (savedWindow.valid) {
        rect = fitWindowToWorkArea(savedWindow, work, GetDpiForWindow(window));
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (show != SW_HIDE && show != SW_SHOWMINIMIZED && show != SW_SHOWMINNOACTIVE)
            show = savedWindow.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.hwnd == window || IsChild(window, msg.hwnd)) && msg.wParam == VK_F8 &&
            GetMessageExtraInfo() == static_cast<LPARAM>(0x4354494D4552ULL)) {
            if (msg.message == WM_KEYDOWN) {
                observedDown = qpc();
                ++observedDownCount;
            }
            if (msg.message == WM_KEYUP) {
                observedUp = qpc();
                ++observedUpCount;
            }
        }
        if (!IsDialogMessageW(sourceWindow ? sourceWindow : window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    engine.reset();
    WSACleanup();
    DeleteObject(backgroundBrush);
    DeleteObject(readyBrush);
    DeleteObject(scheduledBrush);
    if (instanceMutex)
        CloseHandle(instanceMutex);
    for (auto f : {normalFont, titleFont, errorFont, monoFont, bannerFont})
        if (f)
            DeleteObject(f);
    if (largeAppIcon)
        DestroyIcon(largeAppIcon);
    if (smallAppIcon)
        DestroyIcon(smallAppIcon);
    return static_cast<int>(msg.wParam);
}
