#pragma once

// The one SNTP exchange EspSntp makes (RFC 4330): a version 4 client
// request and the checks on its reply. Header-only and free of ESP-IDF
// includes, so tests/port_esp/sntp_packet_test.cpp runs it on the host.
//
// The request's transmit timestamp is a random nonce rather than the
// (possibly unset) local clock; a server copies it into the reply's
// originate timestamp, which is how a reply to some other request, or a
// forged one, is told apart.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wisp::port::esp {

inline constexpr size_t kSntpPacketSize = 48;
using SntpPacket = std::array<uint8_t, kSntpPacketSize>;

// Seconds from 1900-01-01 (the NTP epoch) to 1970-01-01.
inline constexpr int64_t kNtpToUnix = 2208988800LL;

namespace sntp_detail {

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

inline uint64_t be64(const uint8_t* p) { return (uint64_t{be32(p)} << 32) | be32(p + 4); }

}  // namespace sntp_detail

// LI 0, version 4, mode 3 (client), every other field zero but the
// transmit timestamp, which carries `nonce`.
inline void sntp_request(SntpPacket& out, uint64_t nonce) {
    out.fill(0);
    out[0] = (0 << 6) | (4 << 3) | 3;
    for (int i = 0; i < 8; ++i) out[40 + i] = static_cast<uint8_t>(nonce >> (56 - 8 * i));
}

// The reply's transmit time as Unix seconds (rounded to the nearest), or
// nullopt unless it is a usable answer to the request carrying `nonce`:
// at least 48 bytes, mode 4 (server), version 3 or 4, leap indicator not
// 3 (unsynchronized), stratum 1-15 (0 is a kiss-o'-death), originate
// timestamp == nonce and a nonzero transmit timestamp. Seconds with the
// top bit clear are read as NTP era 1 (from 2036-02-07).
inline std::optional<int64_t> sntp_parse_reply(const uint8_t* data, size_t len, uint64_t nonce) {
    using sntp_detail::be32;
    using sntp_detail::be64;
    if (data == nullptr || len < kSntpPacketSize) return std::nullopt;
    const unsigned leap = data[0] >> 6;
    const unsigned version = (data[0] >> 3) & 0x7;
    const unsigned mode = data[0] & 0x7;
    const unsigned stratum = data[1];
    if (mode != 4 || leap == 3 || version < 3 || version > 4) return std::nullopt;
    if (stratum == 0 || stratum > 15) return std::nullopt;
    if (be64(data + 24) != nonce) return std::nullopt;
    const uint32_t seconds = be32(data + 40);
    const uint32_t fraction = be32(data + 44);
    if (seconds == 0 && fraction == 0) return std::nullopt;
    int64_t ntp = seconds;
    if ((seconds & 0x80000000u) == 0) ntp += int64_t{1} << 32;
    if (fraction >= 0x80000000u) ++ntp;
    return ntp - kNtpToUnix;
}

}  // namespace wisp::port::esp
