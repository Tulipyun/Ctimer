#pragma once
#include "platform.hpp"
#include <functional>
#include <span>

namespace ct {
struct NtpResult {
    bool ok{};
    Sample sample;
    std::wstring status, address;
    bool disable{};
    int backoffSeconds{};
    Ns t1{}, t2{}, t3{}, t4{};
    Tick sentQpc{}, receivedQpc{};
    bool sent{}, deferred{}, stored{};
};
std::uint64_t encodeNtp(Ns utc);
std::optional<Ns> decodeNtp(std::uint64_t timestamp, Ns reference);
NtpResult parseNtp(std::span<const unsigned char> packet, std::uint64_t originate, Ns t1, Ns t4, Tick q,
                   std::size_t index, const Source& source);
NtpResult queryNtp(const Source& source, std::size_t index, const ClockModel& baseline,
                   const std::function<bool()>& cancelled, const std::wstring& preferredAddress = {},
                   const std::wstring& avoidedAddress = {},
                   const std::function<bool(const std::wstring&)>& acquireEndpoint = {});
} // namespace ct
