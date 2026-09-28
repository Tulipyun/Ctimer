#include "ntp.hpp"
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace ct {
constexpr std::int64_t NtpEpoch = 2208988800LL;
std::uint64_t encodeNtp(Ns utc) {
    auto sec = utc / Second + NtpEpoch;
    auto fraction = static_cast<std::uint64_t>(utc % Second) * (1ULL << 32) / Second;
    return (static_cast<std::uint64_t>(sec) << 32) | fraction;
}
std::optional<Ns> decodeNtp(std::uint64_t timestamp, Ns reference) {
    if (timestamp == 0)
        return {};
    const std::int64_t ref = reference / Second + NtpEpoch;
    std::int64_t sec = (ref & ~0xFFFFFFFFLL) + static_cast<std::int64_t>(timestamp >> 32);
    if (sec - ref > (1LL << 31))
        sec -= (1LL << 32);
    if (ref - sec > (1LL << 31))
        sec += (1LL << 32);
    const auto unixSec = sec - NtpEpoch;
    // Explicit range prevents signed nanosecond overflow and an implausible era.
    if (unixSec < 0 || unixSec > 4102444800LL)
        return {};
    return unixSec * Second + static_cast<Ns>((timestamp & 0xFFFFFFFFULL) * Second / (1ULL << 32));
}
static std::uint32_t read32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
static std::uint64_t read64(const unsigned char* p) {
    return (std::uint64_t(read32(p)) << 32) | read32(p + 4);
}
static void write64(unsigned char* p, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<unsigned char>(v);
        v >>= 8;
    }
}
NtpResult parseNtp(std::span<const unsigned char> packet, std::uint64_t originate, Ns t1, Ns t4, Tick q,
                   std::size_t index, const Source& source) {
    NtpResult r;
    r.t1 = t1;
    r.t4 = t4;
    auto fail = [&](const wchar_t* s) {
        r.status = s;
        return r;
    };
    if (packet.size() < 48 || packet.size() > 1024)
        return fail(L"无效 NTP 长度");
    if ((packet[0] & 7) != 4 || ((packet[0] >> 3) & 7) < 3 || ((packet[0] >> 3) & 7) > 4)
        return fail(L"无效版本/服务器模式");
    if (read64(packet.data() + 24) != originate)
        return fail(L"响应不匹配/重复响应");
    if (packet[1] == 0) {
        std::string code(reinterpret_cast<const char*>(packet.data() + 12), 4);
        r.status = L"KoD " + wide(code);
        r.backoffSeconds = 1024;
        r.disable = code == "DENY" || code == "RSTR";
        return r;
    }
    if ((packet[0] >> 6) == 3 || packet[1] > 15)
        return fail(L"服务器未同步");
    // This initial client supports only basic unauthenticated NTP; do not misparse NTS/MAC data.
    if (packet.size() != 48)
        return fail(L"首版不支持 NTP 扩展/MAC 响应");
    auto t2 = decodeNtp(read64(packet.data() + 32), t1), t3 = decodeNtp(read64(packet.data() + 40), t1);
    if (!t2 || !t3 || *t3 < *t2 || *t3 - *t2 > 2 * Second || t4 < t1)
        return fail(L"异常时间戳");
    r.t2 = *t2;
    r.t3 = *t3;
    double rtt = static_cast<double>((t4 - t1) - (*t3 - *t2)) / Millisecond;
    if (rtt < -0.001 || rtt > 1500)
        return fail(L"RTT 超出允许范围");
    const double rootDelay = static_cast<std::int32_t>(read32(packet.data() + 4)) / 65536.0 * 1000;
    const double dispersion = read32(packet.data() + 8) / 65536.0 * 1000;
    const int precisionExponent = static_cast<std::int8_t>(packet[3]);
    if (std::abs(rootDelay) > 10000 || dispersion > 10000 || precisionExponent > 0 || precisionExponent < -63)
        return fail(L"服务器误差声明异常");
    r.sample = {index,
                source.group,
                q,
                static_cast<double>((*t2 - t1) + (*t3 - t4)) / (2 * Millisecond),
                std::max(0.0, rtt),
                std::max(0.0, rootDelay) / 2 + dispersion + std::ldexp(1000.0, precisionExponent) + 0.05,
                packet[1]};
    r.ok = true;
    r.status = L"有效样本";
    return r;
}

NtpResult queryNtp(const Source& source, std::size_t index, const ClockModel& baseline,
                   const std::function<bool()>& cancelled, const std::wstring& preferredAddress,
                   const std::wstring& avoidedAddress,
                   const std::function<bool(const std::wstring&)>& acquireEndpoint) {
    NtpResult r;
    using CancelLookup = INT(WSAAPI*)(LPHANDLE);
    using LookupResult = INT(WSAAPI*)(LPOVERLAPPED);
    // Some MinGW SDK versions omit these declarations; use the documented Windows exports.
    auto cancelLookup = reinterpret_cast<CancelLookup>(
        GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), "GetAddrInfoExCancel"));
    auto lookupResult = reinterpret_cast<LookupResult>(
        GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), "GetAddrInfoExOverlappedResult"));
    if (!cancelLookup || !lookupResult) {
        r.status = L"系统缺少可取消 DNS API";
        return r;
    }
    if (cancelled()) {
        r.status = L"已停止";
        return r;
    }
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    PADDRINFOEXW addresses = nullptr;
    HANDLE lookup = nullptr;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        r.status = L"DNS 事件创建失败";
        return r;
    }
    TIMEVAL timeout{2, 0};
    int status = GetAddrInfoExW(source.host.c_str(), std::to_wstring(source.port).c_str(), NS_DNS, nullptr,
                                &hints, &addresses, &timeout, &overlapped, nullptr, &lookup);
    if (status == WSA_IO_PENDING) {
        const auto limit = qpc() + 3 * qpcFrequency();
        while (WaitForSingleObject(overlapped.hEvent, 30) == WAIT_TIMEOUT) {
            if (cancelled() || qpc() >= limit) {
                cancelLookup(&lookup);
                WaitForSingleObject(overlapped.hEvent, INFINITE);
                break;
            }
        }
        status = lookupResult(&overlapped);
    }
    CloseHandle(overlapped.hEvent);
    if (status != 0 || !addresses || cancelled()) {
        if (addresses)
            FreeAddrInfoExW(addresses);
        r.status = cancelled() ? L"已停止" : L"DNS 失败: " + std::to_wstring(status);
        return r;
    }
    auto numericAddress = [](PADDRINFOEXW p) {
        wchar_t text[128]{};
        GetNameInfoW(p->ai_addr, static_cast<socklen_t>(p->ai_addrlen), text, 128, nullptr, 0,
                     NI_NUMERICHOST);
        return std::wstring(text);
    };
    std::vector<PADDRINFOEXW> choices;
    for (auto p = addresses; p; p = p->ai_next) {
        if (p->ai_family == AF_INET || p->ai_family == AF_INET6)
            choices.push_back(p);
    }
    // Stable order lets consecutive failures visit all addresses, rather than
    // indefinitely alternating the first two entries of a shuffled DNS reply.
    std::sort(choices.begin(), choices.end(), [&](auto a, auto b) {
        if (a->ai_family != b->ai_family)
            return a->ai_family == AF_INET;
        return numericAddress(a) < numericAddress(b);
    });
    auto preferred = std::find_if(choices.begin(), choices.end(),
                                  [&](auto p) { return numericAddress(p) == preferredAddress; });
    auto avoided = std::find_if(choices.begin(), choices.end(),
                                [&](auto p) { return numericAddress(p) == avoidedAddress; });
    auto selected = choices.empty() ? addresses : choices.front();
    if (preferred != choices.end())
        selected = *preferred;
    else if (avoided != choices.end())
        selected = choices[(static_cast<std::size_t>(avoided - choices.begin()) + 1) % choices.size()];
    wchar_t numeric[128]{};
    GetNameInfoW(selected->ai_addr, static_cast<socklen_t>(selected->ai_addrlen), numeric, 128, nullptr, 0,
                 NI_NUMERICHOST);
    r.address = numeric;
    SOCKET socket = ::socket(selected->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        FreeAddrInfoExW(addresses);
        r.status = L"Socket 创建失败";
        return r;
    }
    int connected = connect(socket, selected->ai_addr, static_cast<int>(selected->ai_addrlen));
    FreeAddrInfoExW(addresses);
    if (connected != 0) {
        closesocket(socket);
        r.status = L"UDP 连接失败";
        return r;
    }
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    std::array<unsigned char, 48> request{};
    request[0] = 0x23;
    request[2] = 6;
    request[3] = static_cast<unsigned char>(-20);
    const auto frequency = qpcFrequency();
    if (cancelled()) {
        closesocket(socket);
        r.status = L"已停止";
        return r;
    }
    if (acquireEndpoint && !acquireEndpoint(r.address)) {
        closesocket(socket);
        r.deferred = true;
        r.status = L"共享端点/限速：本轮不重复请求";
        return r;
    }
    // All potentially locking checks happen before taking the transmission timestamp.
    Tick sentQpc = qpc();
    Ns sentUtc = baseline.utc(sentQpc, frequency);
    auto stamp = encodeNtp(sentUtc);
    write64(request.data() + 40, stamp);
    r.sentQpc = sentQpc;
    r.t1 = sentUtc;
    if (send(socket, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0) !=
        48) {
        closesocket(socket);
        r.status = L"UDP 发送失败";
        return r;
    }
    r.sent = true;
    const Tick expires = sentQpc + 2 * frequency;
    r.status = L"UDP/123 超时";
    while (!cancelled() && qpc() < expires) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket, &readSet);
        TIMEVAL wait{0, 30000};
        int ready = select(0, &readSet, nullptr, nullptr, &wait);
        if (ready == SOCKET_ERROR) {
            r.status = L"UDP 等待失败";
            break;
        }
        if (ready <= 0)
            continue;
        std::array<unsigned char, 1024> response{};
        const int count =
            recv(socket, reinterpret_cast<char*>(response.data()), static_cast<int>(response.size()), 0);
        const Tick receivedQpc = qpc();
        if (count == SOCKET_ERROR) {
            r.status = L"UDP 接收失败: " + std::to_wstring(WSAGetLastError());
            break;
        }
        // A four-timestamp phase estimate belongs to the exchange midpoint.
        const Tick sampleQpc = sentQpc + (receivedQpc - sentQpc) / 2;
        auto parsed = parseNtp(std::span(response.data(), count), stamp, sentUtc,
                               baseline.utc(receivedQpc, frequency), sampleQpc, index, source);
        parsed.address = r.address;
        parsed.sent = true;
        parsed.sentQpc = sentQpc;
        parsed.receivedQpc = receivedQpc;
        if (!parsed.ok && parsed.status == L"响应不匹配/重复响应")
            continue;
        r = parsed;
        break;
    }
    if (cancelled()) {
        r.ok = false;
        r.status = L"已停止；响应未应用";
    }
    closesocket(socket);
    return r;
}
} // namespace ct
