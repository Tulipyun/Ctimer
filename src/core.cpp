#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <map>
#include <sstream>

namespace ct {
Ns ClockModel::utc(Tick q, Tick frequency) const {
    return anchorUtc + static_cast<Ns>(std::llround(static_cast<long double>(q - anchorQpc) * Second /
                                                    frequency * (1.0L + frequencyPpm / 1e6L)));
}
Tick ClockModel::deadline(Ns t, Tick frequency) const {
    return anchorQpc + static_cast<Tick>(std::ceil(static_cast<long double>(t - anchorUtc) * frequency /
                                                   Second / (1.0L + frequencyPpm / 1e6L)));
}
double ClockModel::uncertainty(Tick q, Tick frequency) const {
    return errorMs + std::abs(static_cast<double>(q - anchorQpc) / frequency) * residualPpm / 1000.0;
}
bool freezeDue(Ns now, Ns target) {
    return target - now <= FreezeLead;
}
Ns nextHourlyTarget(Ns previous, Ns now) {
    Ns next = previous + Hour;
    if (next <= now + 100 * Millisecond)
        next += ((now + 100 * Millisecond - next) / Hour + 1) * Hour;
    return next;
}
std::optional<std::wstring> normalizeTimeFields(std::wstring minutes, std::wstring seconds,
                                                std::wstring milliseconds) {
    auto digits = [](const std::wstring& s) {
        return std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    };
    if (minutes.size() > 2 || seconds.size() > 2 || milliseconds.size() > 3 || !digits(minutes) ||
        !digits(seconds) || !digits(milliseconds))
        return {};
    while (minutes.size() < 2)
        minutes = L"0" + minutes;
    while (seconds.size() < 2)
        seconds = L"0" + seconds;
    while (milliseconds.size() < 3)
        milliseconds += L"0";
    auto joined = minutes + L":" + seconds + L"." + milliseconds;
    if (!parseTime(joined))
        return {};
    return joined;
}

std::optional<TimeOfDay> parseTime(const std::wstring& text) {
    if (text.size() != 9)
        return {};
    const size_t m = 0;
    if (text[2] != L':' || text[5] != L'.')
        return {};
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == m + 2 || i == m + 5)
            continue;
        if (text[i] < L'0' || text[i] > L'9')
            return {};
    }
    auto two = [&](size_t i) { return (text[i] - L'0') * 10 + text[i + 1] - L'0'; };
    TimeOfDay t;
    t.hour = -1;
    t.minute = two(m);
    t.second = two(m + 3);
    t.millisecond = (text[m + 6] - L'0') * 100 + two(m + 7);
    if (t.hour > 23 || t.minute > 59 || t.second > 59)
        return {};
    return t;
}

static std::wstring trim(std::wstring s) {
    const auto a = s.find_first_not_of(L" \r\n\t");
    if (a == std::wstring::npos)
        return {};
    return s.substr(a, s.find_last_not_of(L" \r\n\t") - a + 1);
}
std::vector<Source> defaultSources() {
    auto result = diverseSources();
    auto additional = regionalSources();
    result.insert(result.end(), additional.begin(), additional.end());
    return result;
}
std::vector<Source> regionalSources() {
    return {{L"ntp.aliyun.com", L"aliyun"}, {L"ntp1.aliyun.com", L"aliyun"}, {L"ntp2.aliyun.com", L"aliyun"}};
}
std::vector<Source> diverseSources() {
    return {{L"ntp.ntsc.ac.cn", L"ntsc"},
            {L"ntp.cnnic.cn", L"cnnic"},
            {L"cn.pool.ntp.org", L"pool", 123, 64},
            {L"time.cloudflare.com", L"cloudflare"},
            {L"ntp.nict.jp", L"nict"},
            {L"time.nist.gov", L"nist"},
            {L"ntp.ubuntu.com", L"canonical", 123, 64},
            {L"a.ntp.br", L"nicbr", 123, 64},
            {L"ntp.netnod.se", L"netnod", 123, 64}};
}
std::wstring operatorGroup(const Source& s) {
    // Known operator aliases must not become independent votes through user-entered group names.
    const auto& h = s.host;
    if (h.ends_with(L".aliyun.com"))
        return L"aliyun";
    if (h.ends_with(L".tencent.com"))
        return L"tencent";
    if (h == L"time.cloudflare.com")
        return L"cloudflare";
    if (h.ends_with(L".nict.jp"))
        return L"nict";
    if (h.ends_with(L".nist.gov"))
        return L"nist";
    if (h.ends_with(L".tsinghua.edu.cn"))
        return L"tsinghua";
    if (h.ends_with(L".ntsc.ac.cn"))
        return L"ntsc";
    if (h.ends_with(L".cnnic.cn"))
        return L"cnnic";
    if (h.ends_with(L".pool.ntp.org") || h == L"pool.ntp.org")
        return L"pool";
    if (h == L"ntp.ubuntu.com")
        return L"canonical";
    if (h.ends_with(L".ntp.br"))
        return L"nicbr";
    if (h == L"ntp.se" || h.ends_with(L".netnod.se"))
        return L"netnod";
    if (h.ends_with(L".ptb.de"))
        return L"ptb";
    return s.group.empty() ? h : s.group;
}
bool addRegionalSources(std::vector<Source>& sources) {
    bool changed = false;
    for (const auto& recommended : regionalSources()) {
        if (sources.size() >= 24)
            break;
        if (std::none_of(sources.begin(), sources.end(), [&](const Source& s) {
                return s.host == recommended.host && s.port == recommended.port;
            })) {
            sources.push_back(recommended);
            changed = true;
        }
    }
    for (auto& s : sources) {
        auto group = operatorGroup(s);
        if (group != s.group) {
            s.group = group;
            changed = true;
        }
    }
    return changed;
}
bool addDiverseSources(std::vector<Source>& sources) {
    bool changed = false;
    for (const auto& source : diverseSources()) {
        if (sources.size() >= 24)
            break;
        if (std::none_of(sources.begin(), sources.end(),
                         [&](const Source& s) { return s.host == source.host && s.port == source.port; })) {
            sources.push_back(source);
            changed = true;
        }
    }
    return changed;
}
std::wstring serializeSources(const std::vector<Source>& sources) {
    std::wostringstream out;
    for (const auto& s : sources) {
        if (!s.enabled)
            out << L"!";
        if (s.host.find(L':') != std::wstring::npos)
            out << L"[" << s.host << L"]";
        else
            out << s.host;
        if (s.port != 123)
            out << L":" << s.port;
        out << L" | " << s.group << L" | " << s.minPollSeconds << L"\r\n";
    }
    return out.str();
}
std::optional<std::vector<Source>> parseSources(const std::wstring& text, std::wstring& error) {
    std::vector<Source> result;
    std::wistringstream lines(text);
    std::wstring line;
    int number = 0;
    auto fail = [&](const std::wstring& why) -> std::optional<std::vector<Source>> {
        error = L"第 " + std::to_wstring(number) + L" 行：" + why;
        return {};
    };
    while (std::getline(lines, line)) {
        ++number;
        line = trim(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';')
            continue;
        if (result.size() >= 24)
            return fail(L"最多配置 24 个源。");
        Source s;
        if (line[0] == L'!') {
            s.enabled = false;
            line = trim(line.substr(1));
        }
        const size_t bar = line.find(L'|');
        auto endpoint = trim(line.substr(0, bar));
        if (endpoint.empty())
            return fail(L"缺少域名。");
        std::wstring port;
        if (endpoint[0] == L'[') {
            const auto close = endpoint.find(L']');
            if (close == std::wstring::npos)
                return fail(L"IPv6 地址需要方括号。");
            s.host = endpoint.substr(1, close - 1);
            if (close + 1 < endpoint.size()) {
                if (endpoint[close + 1] != L':')
                    return fail(L"地址格式无效。");
                port = endpoint.substr(close + 2);
                if (port.empty())
                    return fail(L"端口为空。");
            }
        } else {
            auto colon = endpoint.find(L':');
            s.host = endpoint.substr(0, colon);
            if (colon != std::wstring::npos) {
                port = endpoint.substr(colon + 1);
                if (port.empty())
                    return fail(L"端口为空。");
            }
        }
        if (s.host.empty() || s.host.size() > 253)
            return fail(L"域名长度无效。");
        for (auto c : s.host)
            if (!(c < 128 && (std::iswalnum(c) || c == L'.' || c == L'-' || c == L':' || c == L'%')))
                return fail(L"请使用 ASCII 域名或 IP 地址。");
        std::transform(s.host.begin(), s.host.end(), s.host.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        auto integer = [](const std::wstring& v, int& out) {
            if (v.empty() || v.size() > 6)
                return false;
            out = 0;
            for (auto c : v) {
                if (c < L'0' || c > L'9')
                    return false;
                out = out * 10 + c - L'0';
            }
            return true;
        };
        if (!port.empty() && (!integer(port, s.port) || s.port < 1 || s.port > 65535))
            return fail(L"端口需为 1–65535。");
        if (bar != std::wstring::npos) {
            const auto next = line.find(L'|', bar + 1);
            s.group = trim(line.substr(bar + 1, next == std::wstring::npos ? next : next - bar - 1));
            if (next != std::wstring::npos && (!integer(trim(line.substr(next + 1)), s.minPollSeconds) ||
                                               s.minPollSeconds < 4 || s.minPollSeconds > 86400))
                return fail(L"轮询间隔需为 4–86400 秒；请遵守该时间源的请求频率限制。");
        }
        s.group = operatorGroup(s);
        if (s.group.size() > 64)
            return fail(L"分组名称过长。");
        for (const auto& prior : result)
            if (prior.host == s.host && prior.port == s.port)
                return fail(L"重复地址。");
        result.push_back(s);
    }
    if (result.empty()) {
        error = L"至少保留一个时间源。";
        return {};
    }
    return result;
}

double median(std::vector<double> v) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
SyncPhase syncPhase(bool syncing, bool frozen, bool active, bool refiningEnabled, Ns now, Ns target,
                    int refinementLeadSeconds) {
    if (frozen)
        return SyncPhase::Frozen;
    if (!syncing)
        return SyncPhase::Paused;
    if (active && refiningEnabled && target - now > FreezeLead &&
        target - now <= static_cast<Ns>(refinementLeadSeconds) * Second)
        return SyncPhase::Refining;
    return SyncPhase::Tracking;
}

Interval sampleInterval(const Sample& sample) {
    if (sample.interval)
        return *sample.interval;
    const double radius = std::max(0.05, sample.rootDistanceMs + sample.rttMs / 2);
    return {sample.offsetMs - radius, sample.offsetMs + radius};
}
double sampleQuality(const Sample& sample) {
    auto bounds = sampleInterval(sample);
    return std::max(0.05, std::max(sample.offsetMs - bounds.lower, bounds.upper - sample.offsetMs) +
                              2 * sample.jitterMs + sample.failurePenaltyMs);
}
IntervalConsensus selectIntervals(const std::vector<Interval>& intervals, int requestedFaults) {
    IntervalConsensus result;
    result.requestedFaults = std::max(0, requestedFaults);
    const int n = static_cast<int>(intervals.size());
    if (!n)
        return result;
    result.faultToleranceReady = result.requestedFaults > 0 && n >= 2 * result.requestedFaults + 1;
    result.toleratedFaults = result.faultToleranceReady ? result.requestedFaults : 0;
    result.quorum = n - result.toleratedFaults;
    struct Event {
        int starts{}, ends{};
    };
    std::map<double, Event> events;
    for (const auto& interval : intervals) {
        if (!std::isfinite(interval.lower) || !std::isfinite(interval.upper) ||
            interval.lower > interval.upper)
            return result;
        ++events[interval.lower].starts;
        ++events[interval.upper].ends;
    }
    auto append = [&](double low, double high) {
        if (!result.regions.empty() && low <= result.regions.back().upper)
            result.regions.back().upper = std::max(result.regions.back().upper, high);
        else
            result.regions.push_back({low, high});
    };
    int active = 0;
    for (auto it = events.begin(); it != events.end(); ++it) {
        const double coordinate = it->first;
        // Closed intervals: starts join before endpoint coverage is checked; ends leave afterwards.
        const int at = active + it->second.starts;
        if (at >= result.quorum)
            append(coordinate, coordinate);
        active = at - it->second.ends;
        auto next = std::next(it);
        if (next != events.end() && active >= result.quorum)
            append(coordinate, next->first);
    }
    if (result.regions.empty())
        return result;
    result.envelope = {result.regions.front().lower, result.regions.back().upper};
    result.ambiguous = result.regions.size() > 1;
    result.valid = !result.ambiguous;
    return result;
}
Estimate combineSources(const std::vector<Sample>& samples, int requestedFaults) {
    std::map<std::wstring, Sample> byGroup;
    for (const auto& s : samples) {
        const auto bounds = sampleInterval(s);
        if (!std::isfinite(s.offsetMs) || !std::isfinite(s.rttMs) || !std::isfinite(s.rootDistanceMs) ||
            !std::isfinite(s.jitterMs) || !std::isfinite(s.failurePenaltyMs) ||
            !std::isfinite(bounds.lower) || !std::isfinite(bounds.upper) || bounds.lower > bounds.upper ||
            s.rttMs < 0 || s.rootDistanceMs < 0 || s.jitterMs < 0 || s.failurePenaltyMs < 0)
            continue;
        auto found = byGroup.find(s.group);
        if (found == byGroup.end() || sampleQuality(s) < sampleQuality(found->second))
            byGroup[s.group] = s;
    }
    Estimate e;
    e.availableGroups = static_cast<int>(byGroup.size());
    std::vector<Sample> candidates;
    std::vector<Interval> intervals;
    for (const auto& [_, s] : byGroup) {
        candidates.push_back(s);
        intervals.push_back(sampleInterval(s));
    }
    const auto consensus = selectIntervals(intervals, requestedFaults);
    e.ambiguous = consensus.ambiguous;
    e.safeInterval = consensus.envelope;
    e.toleratedFaults = consensus.toleratedFaults;
    e.quorum = consensus.quorum;
    e.faultToleranceReady = consensus.faultToleranceReady;
    if (!consensus.valid)
        return e;
    const double center = (e.safeInterval.lower + e.safeInterval.upper) / 2;
    std::vector<Sample> selected;
    for (const auto& s : candidates) {
        auto bounds = sampleInterval(s);
        if (bounds.lower <= center && center <= bounds.upper)
            selected.push_back(s);
    }
    e.consensusGroups = static_cast<int>(selected.size());
    if (e.consensusGroups < e.quorum)
        return e;
    std::sort(selected.begin(), selected.end(),
              [](const auto& a, const auto& b) { return sampleQuality(a) < sampleQuality(b); });
    const double limit = std::max(5.0, sampleQuality(selected.front()) * 2.5);
    selected.erase(std::remove_if(selected.begin(), selected.end(),
                                  [&](const auto& s) { return sampleQuality(s) > limit; }),
                   selected.end());
    if (selected.size() > 3)
        selected.resize(3);
    e.referenceSource = selected.front().source;
    double total = 0, weighted = 0;
    for (const auto& s : selected) {
        const double weight = 1 / (sampleQuality(s) * sampleQuality(s));
        weighted += s.offsetMs * weight;
        total += weight;
        e.usedSources.push_back(s.source);
    }
    e.offsetMs = std::clamp(weighted / total, e.safeInterval.lower, e.safeInterval.upper);
    // Quality pruning affects the point estimate only. Never shrink the fault-model envelope.
    e.uncertaintyMs = std::max({0.05, e.offsetMs - e.safeInterval.lower, e.safeInterval.upper - e.offsetMs});
    std::vector<double> deviations;
    for (const auto& s : selected)
        deviations.push_back(std::abs(s.offsetMs - e.offsetMs));
    e.jitterMs = 1.4826 * median(deviations);
    e.groups = static_cast<int>(selected.size());
    e.valid = true;
    return e;
}
std::optional<double> fitFrequency(const std::vector<PhasePoint>& points) {
    if (points.size() < 6 || points.back().seconds - points.front().seconds < 300)
        return {};
    std::vector<double> slopes;
    for (size_t i = 0; i < points.size(); ++i)
        for (size_t j = i + 1; j < points.size(); ++j) {
            double dt = points[j].seconds - points[i].seconds;
            if (dt >= 120)
                slopes.push_back((points[j].offsetMs - points[i].offsetMs) / dt * 1000.0);
        }
    const auto ppm = median(slopes);
    if (slopes.empty() || !std::isfinite(ppm) || std::abs(ppm) > 200)
        return {};
    return ppm;
}
} // namespace ct
