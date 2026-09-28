#include "engine.hpp"
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
#include <set>

using namespace ct;
static int failures{}, checks{};
void runEstimatorTests();
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
void stamp(unsigned char* p, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<unsigned char>(v);
        v >>= 8;
    }
}
class MockNtp {
    SOCKET socket_{};
    std::jthread thread_;
    std::atomic<bool> quit_{};

  public:
    int port{};
    std::atomic<int> requests{};
    std::atomic<bool> responding{true};
    explicit MockNtp(int delayMs = 0) {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        int n = sizeof(local);
        getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &n);
        port = ntohs(local.sin_port);
        DWORD timeout = 100;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        thread_ = std::jthread([this, delayMs] {
            const auto clock = initialClock();
            while (!quit_) {
                unsigned char request[48];
                sockaddr_storage peer{};
                int length = sizeof(peer);
                int count = recvfrom(socket_, reinterpret_cast<char*>(request), 48, 0,
                                     reinterpret_cast<sockaddr*>(&peer), &length);
                if (count != 48)
                    continue;
                ++requests;
                if (!responding)
                    continue;
                auto received = clock.utc(qpc(), qpcFrequency()) + 12 * Millisecond;
                if (delayMs)
                    Sleep(delayMs);
                unsigned char reply[48]{};
                reply[0] = 0x24;
                reply[1] = 1;
                reply[3] = static_cast<unsigned char>(-20);
                std::copy(request + 40, request + 48, reply + 24);
                stamp(reply + 32, encodeNtp(received));
                stamp(reply + 40, encodeNtp(clock.utc(qpc(), qpcFrequency()) + 12 * Millisecond));
                sendto(socket_, reinterpret_cast<char*>(reply), 48, 0, reinterpret_cast<sockaddr*>(&peer),
                       length);
            }
        });
    }
    ~MockNtp() {
        quit_ = true;
        thread_.join();
        closesocket(socket_);
    }
};
void benchmark() {
    HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto f = qpcFrequency();
    auto start = qpc() + f / 10;
    std::vector<double> errors;
    errors.reserve(10000);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (int i = 0; timer && i < 10000; ++i) {
        auto target = start + i * f / 200;
        if (!preciseWait(target, timer, cancel, f, f / 1000))
            break;
        errors.push_back((qpc() - target) * 1000.0 / f);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    std::sort(errors.begin(), errors.end());
    std::ofstream report("build/benchmark.json");
    if (!errors.empty()) {
        auto percentile = [&](double p) {
            return errors[static_cast<size_t>(std::ceil(p * errors.size())) - 1];
        };
        report << "{\"samples\":" << errors.size()
               << ",\"metric\":\"QPC wait lateness, not UTC accuracy or "
                  "SendInput\",\"priority\":\"THREAD_PRIORITY_HIGHEST\",\"p50_ms\":"
               << percentile(.5) << ",\"p99_ms\":" << percentile(.99) << ",\"p999_ms\":" << percentile(.999)
               << ",\"max_ms\":" << errors.back() << ",\"over_1ms\":"
               << std::count_if(errors.begin(), errors.end(), [](double e) { return e > 1; }) << "}";
        std::cout << "Wait benchmark: n=" << errors.size() << " p99=" << percentile(.99)
                  << " ms max=" << errors.back() << " ms\n";
    }
    if (timer)
        CloseHandle(timer);
    CloseHandle(cancel);
}
int main(int argc, char** argv) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    if (argc == 3 && std::string(argv[1]) == "--observe") {
        const int seconds = std::atoi(argv[2]);
        if (seconds < 10 || seconds > 3600)
            return 2;
        const auto directory = std::filesystem::path("build") / ("ntp-observation-" + std::to_string(qpc()));
        std::cout << "Read-only NTP observation: " << directory.string() << std::endl;
        {
            Config config;
            config.autoSystemClock = config.autoElevate = config.autoSchedule = false;
            Engine engine(config, directory);
            const auto start = qpc();
            for (int elapsed = 0; elapsed < seconds; ++elapsed) {
                Sleep(1000);
                if ((elapsed + 1) % 10 == 0) {
                    auto snapshot = engine.snapshot();
                    std::cout << "t=" << (qpc() - start) / engine.frequency
                              << " samples=" << snapshot.validSamples << " model=" << snapshot.clock.version
                              << " primary=" << utf8(snapshot.primarySource)
                              << " error_ms=" << snapshot.clock.errorMs
                              << " rate_ppm=" << snapshot.clock.frequencyPpm
                              << " mature=" << snapshot.frequencyMature << std::endl;
                }
            }
            engine.shutdown();
        }
        WSACleanup();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--survey") {
        std::ofstream out("build/source-survey.json");
        out << "[";
        bool first = true;
        std::set<std::wstring> queried;
        for (const auto& source : defaultSources()) {
            auto r = queryNtp(
                source, 0, initialClock(), [] { return false; }, L"", L"",
                [&](const std::wstring& address) {
                    return queried.insert(address + L":" + std::to_wstring(source.port)).second;
                });
            if (!first)
                out << ",";
            first = false;
            out << "{\"host\":\"" << utf8(source.host) << "\",\"valid\":" << (r.ok ? "true" : "false")
                << ",\"rtt_ms\":" << r.sample.rttMs << ",\"offset_ms\":" << r.sample.offsetMs
                << ",\"stratum\":" << r.sample.stratum
                << ",\"shared_endpoint_skipped\":" << (r.deferred ? "true" : "false") << "}";
            std::cout << utf8(source.host) << " valid=" << r.ok << " rtt=" << r.sample.rttMs << " ms\n";
        }
        out << "]";
        WSACleanup();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--benchmark") {
        benchmark();
        WSACleanup();
        return 0;
    }
    runEstimatorTests();
    check(parseTime(L"59:59.123")->millisecond == 123, "three-digit millisecond input");
    check(!parseTime(L"23:59:59.999"), "hour input rejected");
    check(normalizeTimeFields(L"5", L"", L"1") == L"05:00.100",
          "three fields pad minute and fractional milliseconds");
    check(normalizeTimeFields(L"", L"5", L"12") == L"00:05.120", "fractional milliseconds pad on right");
    check(normalizeTimeFields(L"", L"", L"") == L"00:00.000", "empty numeric fields default to zero");
    check(normalizeTimeFields(L"0", L"0", L"001") == L"00:00.001",
          "three millisecond digits retain exact value");
    check(!normalizeTimeFields(L"60", L"0", L"0") && !normalizeTimeFields(L"0", L"0", L"1000"),
          "invalid numeric fields rejected");
    check(nextHourlyTarget(59 * Second, 60 * Second) == Hour + 59 * Second,
          "next hour appointment after completion");
    check(nextHourlyTarget(59 * Second, 3 * Hour) == 3 * Hour + 59 * Second,
          "missed hours skipped without catch up input");
    check(Source{}.minPollSeconds == 10, "default poll interval is ten seconds");
    for (auto s : {L"59:59:1234", L"59:59.1234", L"59:59.1", L"60:00.000", L"24:00:00.000", L"10:60.000",
                   L"-1:00.000"})
        check(!parseTime(s), "invalid time rejected");
    check(freezeDue(0, 60 * Second), "freeze exact 60s");
    check(!freezeDue(0, 60 * Second + 1), "no freeze before cutoff");
    check(freezeDue(0, 5 * Second), "arm inside window freezes immediately");
    AutoScheduleDraft draft;
    check(!draft.pending, "startup does not schedule a default time");
    draft.changed(100, 1000, 123);
    check(!draft.ready(1099) && draft.ready(1100), "automatic scheduling debounced for one second");
    draft.cancel();
    check(!draft.ready(10000), "cancel does not automatically rearm");
    draft.changed(20000, 1000, 789);
    check(draft.ready(21000) && draft.hourReference == 789,
          "user edit creates new explicit scheduling intent");
    ClockModel clock{1000000, 1800000000000000000LL, 30, .2, 30, true, 7};
    Tick frequency = 10000000;
    auto future = clock.utc(clock.anchorQpc + 60 * frequency, frequency);
    check(std::llabs(future - clock.anchorUtc - 60 * Second - 1800000) < 2, "frequency correction");
    check(clock.deadline(future, frequency) >= clock.anchorQpc + 60 * frequency, "deadline rounds upward");
    check(std::abs(clock.uncertainty(clock.anchorQpc + 60 * frequency, frequency) - 2.0) < 1e-6,
          "60 second holdover error growth");
    std::wstring error;
    auto sources = parseSources(
        L"time.cloudflare.com | cf | 64\n!ntp.nict.jp | nict | 64\n127.0.0.1:1123 | local | 4", error);
    check(sources && sources->size() == 3 && !(*sources)[1].enabled && (*sources)[2].port == 1123,
          "source configuration syntax");
    check(!parseSources(L"x | a | 0", error), "minimum poll rejected");
    check(!parseSources(L"X|a|64\nx|b|64", error), "duplicate host case rejected");
    auto aliases =
        parseSources(L"ntp.tencent.com|one|64\nntp1.tencent.com|two|64\nntp4.tencent.com|three|64", error);
    check(aliases && (*aliases)[0].group == L"tencent" && (*aliases)[2].group == L"tencent",
          "known operator aliases share one vote");
    auto migration = defaultSources();
    auto n = migration.size();
    addRegionalSources(migration);
    check(migration.size() == n, "source migration is idempotent");
    check(keyPlan(L"CTRL+F8")->count == 2, "combination input");
    check(!keyPlan(L"CTRL+CTRL"), "duplicate key rejected");
    check(!keyPlan(L"F25"), "invalid function key rejected");
    check(!keyPlan(L"F8+"), "trailing key separator rejected");
    auto click = mousePlan(0);
    check(click.down[0].mi.dwFlags == MOUSEEVENTF_LEFTDOWN && click.up[0].mi.dwFlags == MOUSEEVENTF_LEFTUP,
          "complete click operation");
    auto original = 1800000000123456789LL;
    check(std::llabs(*decodeNtp(encodeNtp(original), original) - original) <= 1, "NTP roundtrip");
    auto era = 2114380800123000000LL;
    check(std::llabs(*decodeNtp(encodeNtp(era), era) - era) <= 1, "NTP 2036 era rollover");
    Source source{L"mock", L"mock"};
    unsigned char packet[48]{};
    packet[0] = 0x24;
    packet[1] = 1;
    packet[3] = static_cast<unsigned char>(-20);
    auto origin = encodeNtp(original);
    stamp(packet + 24, origin);
    stamp(packet + 32, encodeNtp(original + 10 * Millisecond));
    stamp(packet + 40, encodeNtp(original + 11 * Millisecond));
    auto parsed = parseNtp(packet, origin, original, original + 5 * Millisecond, 0, 0, source);
    // Integer conversion of NTP's binary fraction contributes at most a few nanoseconds.
    check(parsed.ok && std::abs(parsed.sample.offsetMs - 8) < .000002 &&
              std::abs(parsed.sample.rttMs - 4) < .000002,
          "four timestamp offset and RTT");
    check(!parseNtp(packet, origin + 1, original, original + 5 * Millisecond, 0, 0, source).ok,
          "mismatched response rejected");
    packet[0] = 0xE4;
    check(!parseNtp(packet, origin, original, original + 5 * Millisecond, 0, 0, source).ok,
          "unsynchronized LI rejected");
    packet[0] = 0x24;
    packet[1] = 0;
    std::copy_n("RATE", 4, packet + 12);
    check(parseNtp(packet, origin, original, original + 5 * Millisecond, 0, 0, source).backoffSeconds >= 64,
          "KoD rate backoff");
    std::vector<Sample> samples = {
        {0, L"a", 0, 1, 2, .2, 1}, {1, L"b", 0, 1.2, 2, .2, 1}, {2, L"bad", 0, 100, 2, .2, 1}};
    auto estimate = combineSources(samples);
    check(estimate.valid && estimate.groups == 2 && std::abs(estimate.offsetMs - 1.1) < .01,
          "majority excludes falseticker");
    samples[1].group = L"a";
    estimate = combineSources(samples);
    check(!estimate.valid, "same group cannot outvote independent source");
    samples = {{0, L"a", 0, 0, 20, .1, 1}, {1, L"b", 0, 0, 20, .1, 1}, {2, L"c", 0, 0, 20, .1, 1}};
    estimate = combineSources(samples);
    check(estimate.uncertaintyMs >= 10, "systematic RTT uncertainty not divided by sample count");
    samples = {
        {0, L"near-a", 0, 1, 2, .1, 1}, {1, L"near-b", 0, 1.2, 3, .1, 1}, {2, L"far", 0, 10, 200, .1, 1}};
    estimate = combineSources(samples);
    check(estimate.valid && estimate.groups == 2 && estimate.consensusGroups == 3 &&
              estimate.referenceSource == 0 && estimate.uncertaintyMs < 5,
          "consistent low delay sources preferred over distant backups");
    samples = {{0, L"slow-local", 0, 0, 100, .1, 1},
               {1, L"fast-a", 0, 1, 2, .1, 1},
               {2, L"fast-b", 0, 1.1, 3, .1, 1}};
    estimate = combineSources(samples);
    check(estimate.referenceSource == 1, "closest to uncalibrated local time is not incorrectly preferred");
    std::vector<PhasePoint> points;
    for (int i = 0; i < 7; ++i)
        points.push_back({i * 60.0, 5 + i * 60 * .03});
    check(fitFrequency(points) && std::abs(*fitFrequency(points) - 30) < 1e-6, "frequency regression");
    {
        MockNtp server;
        Source local{L"127.0.0.1", L"local", server.port, 4};
        auto r = queryNtp(local, 0, initialClock(), [] { return false; });
        check(r.ok && std::abs(r.sample.offsetMs - 12) < 3, "real UDP localhost query");
        check(r.sent && r.receivedQpc > r.sentQpc &&
                  r.sample.qpc == r.sentQpc + (r.receivedQpc - r.sentQpc) / 2,
              "UDP phase observation uses exchange midpoint QPC");
        Config c;
        c.autoSync = false;
        c.autoSystemClock = true;
        c.sources = {local};
        std::atomic<int> clockWrites{};
        Engine e(c, L"build/test-logs", [&](const ClockModel&) {
            ++clockWrites;
            return SystemClockResult{true, false, 0};
        });
        std::wstring message;
        e.startSync(message);
        for (int i = 0; i < 100 && !e.snapshot().clock.calibrated; ++i)
            Sleep(20);
        auto s = e.snapshot();
        check(s.clock.calibrated, "engine applies network estimate");
        check(clockWrites == 1, "automatic system clock writer invoked outside freeze (mock only)");
        Job j;
        j.onlyWindow = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));
        j.target = s.clock.utc(qpc(), qpcFrequency()) + 60 * Second + 150 * Millisecond;
        j.input = *keyPlan(L"F8");
        check(e.arm(j, message), "arm before cutoff");
        check(e.snapshot().phase == SyncPhase::Refining, "task enters refinement before freeze");
        check(e.withdrawForEdit(), "pending schedule editable before freeze");
        check(!e.snapshot().active, "editing withdraws pending task");
        check(e.arm(j, message), "rearm edited schedule");
        Sleep(350);
        s = e.snapshot();
        check(s.frozen && !s.syncing, "automatic 60 second freeze");
        check(s.phase == SyncPhase::Frozen, "refinement ends at freeze boundary");
        check(s.preparationSerial == 1 && !s.actionSummary.empty(),
              "one preparation notification with action description");
        check(!e.withdrawForEdit(), "preparing schedule cannot be silently changed");
        auto version = s.clock.version;
        check(!e.startSync(message), "sync restart rejected while frozen");
        Sleep(100);
        check(e.snapshot().clock.version == version, "frozen clock immutable");
        check(clockWrites == 1, "system clock writer not invoked while frozen");
        e.cancel();
        for (int i = 0; i < 100 && e.snapshot().active; ++i)
            Sleep(10);
        check(!e.snapshot().active, "cancel does not wait 60 seconds");
        check(e.snapshot().syncing && !e.snapshot().frozen, "automatic sync resumes after cancellation");
        e.stopSync();
        auto paused = e.snapshot();
        j.target = paused.clock.utc(qpc(), qpcFrequency()) + 5 * Second;
        check(e.arm(j, message), "arm while manually paused");
        e.cancel();
        for (int i = 0; i < 100 && e.snapshot().active; ++i)
            Sleep(10);
        check(!e.snapshot().syncing, "explicit manual pause survives task end");
    }
    {
        MockNtp server(350);
        Config c;
        c.autoSync = false;
        c.autoSystemClock = true;
        c.sources = {{L"127.0.0.1", L"local", server.port, 4}};
        std::atomic<int> lateWrites{};
        Engine e(c, L"build/test-late-logs", [&](const ClockModel&) {
            ++lateWrites;
            return SystemClockResult{true, false, 0};
        });
        std::wstring message;
        e.startSync(message);
        for (int i = 0; i < 100 && server.requests == 0; ++i)
            Sleep(5);
        Job j;
        j.probe = true;
        j.onlyWindow = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));
        j.input = *keyPlan(L"F8");
        j.target = e.snapshot().clock.utc(qpc(), qpcFrequency()) + 60 * Second + 50 * Millisecond;
        check(e.arm(j, message), "arm with pending query");
        Sleep(500);
        auto s = e.snapshot();
        check(s.frozen && s.clock.version == 0 && !s.clock.calibrated,
              "late response cannot calibrate after freeze");
        check(lateWrites == 0, "late response cannot write system clock");
        e.cancel();
    }
    {
        MockNtp server;
        Config config;
        config.sources = {{L"127.0.0.1", L"one", server.port, 4}, {L"localhost", L"two", server.port, 4}};
        Engine engine(config, L"build/refinement-test");
        for (int i = 0; i < 100 && !engine.snapshot().clock.calibrated; ++i)
            Sleep(20);
        Sleep(150);
        auto initial = engine.snapshot();
        check(server.requests == 1 && initial.availableGroups == 1,
              "shared endpoint is queried and counted only once");
        check(initial.sources[0].failures == 0 && initial.sources[1].failures == 0,
              "deferred duplicate does not count as a source failure");
        Job job;
        job.onlyWindow = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));
        job.input = *keyPlan(L"F8");
        job.target = initial.clock.utc(qpc(), qpcFrequency()) + 100 * Second;
        std::wstring message;
        check(engine.arm(job, message), "arm safe refinement integration test");
        for (int i = 0; i < 260 && engine.snapshot().refinementSamples == 0; ++i)
            Sleep(20);
        auto refining = engine.snapshot();
        check(refining.phase == SyncPhase::Refining && refining.refinementSamples > 0 &&
                  refining.clock.version > initial.clock.version,
              "refinement receives new samples and commits a model");
        check(refining.refinementProgress > 0 && refining.refinementProgress < 1,
              "refinement reports time-window progress");
        engine.stopSync();
        const auto pausedVersion = engine.snapshot().clock.version;
        Sleep(150);
        check(engine.snapshot().phase == SyncPhase::Paused &&
                  engine.snapshot().clock.version == pausedVersion,
              "manual pause stops refinement model updates");
        check(engine.startSync(message), "resume refinement manually");
        engine.setRefinement(false);
        check(engine.snapshot().phase == SyncPhase::Tracking,
              "refinement checkbox can leave refinement phase");
        engine.setRefinement(true);
        check(engine.snapshot().phase == SyncPhase::Refining,
              "refinement checkbox can restore refinement phase");
        engine.cancel();
        for (int i = 0; i < 100 && engine.snapshot().active; ++i)
            Sleep(10);
        check(engine.snapshot().records.empty(), "refinement verification sends no input");
        check(engine.configure(config, message) && !engine.snapshot().frequencyMature &&
                  engine.snapshot().refinementSamples == 0 && !engine.snapshot().clock.calibrated,
              "configuration reload clears estimator maturity and phase history");
        engine.sessionInterrupted();
        auto interrupted = engine.snapshot();
        check(!interrupted.clock.calibrated && interrupted.lastSuccessfulSyncQpc == 0 &&
                  interrupted.clock.frequencyPpm == 0 && !interrupted.frequencyMature,
              "power or session interruption invalidates learned clock history");
    }
    std::filesystem::create_directories(L"build/config-test");
    Config c;
    c.sources = {{L"127.0.0.1", L"local", 123, 4}};
    c.hasSchedule = true;
    c.scheduleMinute = 5;
    c.scheduleSecond = 7;
    c.scheduleMillisecond = 100;
    c.actionKind = 0;
    c.key = L"CTRL+F8";
    c.autoElevate = true;
    c.preRefine = false;
    c.faultBudget = 2;
    c.refinementLeadSeconds = 240;
    c.refinementPollSeconds = 16;
    check(saveConfig(L"build/config-test/Ctimer.ini", c, error), "atomic config write");
    Config loaded;
    check(loadConfig(L"build/config-test/Ctimer.ini", loaded, error) && loaded.sources[0].minPollSeconds == 4,
          "UTF8 configuration reload");
    check(loaded.hasSchedule && loaded.scheduleMinute == 5 && loaded.scheduleSecond == 7 &&
              loaded.scheduleMillisecond == 100,
          "appointment time persists across reload");
    check(loaded.actionKind == 0 && loaded.key == L"CTRL+F8" && loaded.autoElevate,
          "action and elevation preferences persist");
    check(!loaded.preRefine && loaded.faultBudget == 2 && loaded.refinementLeadSeconds == 240 &&
              loaded.refinementPollSeconds == 16,
          "estimator and refinement options persist");
    {
        Config layout = c;
        layout.window = {true, true, false, false, -1500, 80, 450, 282, 96};
        check(saveConfig(L"build/config-test/window.ini", layout, error), "save window location");
        Config restored;
        check(loadConfig(L"build/config-test/window.ini", restored, error) && restored.window.valid &&
                  restored.window.x == -1500 && restored.window.y == 80 && restored.window.compact,
              "restore negative monitor coordinates and compact mode");
        RECT leftMonitor{-1920, 0, 0, 1080};
        auto bounds = fitWindowToWorkArea(restored.window, leftMonitor, 96);
        check(bounds.left == -1500 && bounds.top == 80 && bounds.right - bounds.left == 450,
              "unchanged monitor keeps exact window position");
        RECT mainMonitor{0, 40, 1920, 1080};
        bounds = fitWindowToWorkArea(restored.window, mainMonitor, 96);
        check(bounds.left >= mainMonitor.left && bounds.top >= mainMonitor.top &&
                  bounds.right <= mainMonitor.right && bounds.bottom <= mainMonitor.bottom,
              "removed monitor restores window to visible work area");
        restored.window = {true, false, false, false, 100, 100, 600, 400, 96};
        bounds = fitWindowToWorkArea(restored.window, mainMonitor, 144);
        check(bounds.left == 100 && bounds.top == 100 && bounds.right - bounds.left == 900 &&
                  bounds.bottom - bounds.top == 600,
              "saved size scales for changed monitor DPI");
        restored.window.width = 10000;
        restored.window.height = 10000;
        bounds = fitWindowToWorkArea(restored.window, mainMonitor, 192);
        check(bounds.left == mainMonitor.left && bounds.top == mainMonitor.top &&
                  bounds.right == mainMonitor.right && bounds.bottom == mainMonitor.bottom,
              "oversized saved window is bounded by display");
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = DefWindowProcW;
        wc.lpszClassName = L"CtimerPlacementTest";
        RegisterClassW(&wc);
        auto window =
            CreateWindowExW(0, wc.lpszClassName, L"Ctimer placement verification", WS_OVERLAPPEDWINDOW, 130,
                            110, 660, 448, nullptr, nullptr, wc.hInstance, nullptr);
        check(window != nullptr, "create isolated hidden placement test window");
        if (window) {
            auto captured = captureWindowPlacement(window, false, false);
            RECT actual{};
            GetWindowRect(window, &actual);
            check(captured.valid && captured.x == actual.left && captured.y == actual.top &&
                      captured.width == actual.right - actual.left,
                  "capture actual Win32 window bounds");
            Config saved = c;
            saved.window = captured;
            saveConfig(L"build/config-test/actual-window.ini", saved, error);
            DestroyWindow(window);
            Config reopened;
            loadConfig(L"build/config-test/actual-window.ini", reopened, error);
            window = CreateWindowExW(0, wc.lpszClassName, L"Ctimer restored placement", WS_OVERLAPPEDWINDOW,
                                     reopened.window.x, reopened.window.y, reopened.window.width,
                                     reopened.window.height, nullptr, nullptr, wc.hInstance, nullptr);
            GetWindowRect(window, &actual);
            check(actual.left == captured.x && actual.top == captured.y &&
                      actual.right - actual.left == captured.width,
                  "close and recreate window with persisted placement");
            DestroyWindow(window);
        }
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
    if (argc > 2 && std::string(argv[1]) == "--app") {
        auto module = LoadLibraryExW(std::filesystem::path(argv[2]).c_str(), nullptr,
                                     LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        check(module != nullptr, "open built executable as resources without executing it");
        if (module) {
            auto resource = FindResourceW(module, MAKEINTRESOURCEW(100), RT_GROUP_ICON);
            check(resource != nullptr, "application icon is embedded in executable");
            if (resource) {
                auto group = static_cast<const WORD*>(LockResource(LoadResource(module, resource)));
                check(group && group[1] == 1 && group[2] == 9, "embedded icon group has nine resolutions");
            }
            for (int size : {16, 32, 256}) {
                auto icon =
                    static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(100), IMAGE_ICON, size, size, 0));
                check(icon != nullptr, "Windows can load embedded icon at requested size");
                if (icon)
                    DestroyIcon(icon);
            }
            FreeLibrary(module);
        }
    }
    {
        MockNtp server;
        Config config;
        config.sources = {{L"127.0.0.1", L"local", server.port, 4}};
        Engine engine(config, L"build/failure-status-test");
        for (int i = 0; i < 100 && !engine.snapshot().clock.calibrated; ++i)
            Sleep(20);
        const auto initial = engine.snapshot();
        check(initial.lastSuccessfulSyncQpc > 0, "successful sync records its actual timestamp");
        server.responding = false;
        for (int i = 0; i < 400 && !engine.snapshot().lastSyncFailed; ++i)
            Sleep(20);
        auto failed = engine.snapshot();
        check(failed.lastSyncFailed, "network timeout becomes failed synchronization state");
        check(failed.lastSuccessfulSyncQpc == initial.lastSuccessfulSyncQpc &&
                  failed.clock.version == initial.clock.version,
              "failed rounds do not refresh old timestamps or pretend success");
    }
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    WSACleanup();
    return failures ? 1 : 0;
}
