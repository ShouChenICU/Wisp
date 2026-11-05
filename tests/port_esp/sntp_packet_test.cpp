// The ESP port's SNTP request and reply checks (wisp/port/esp/
// sntp_packet.hpp), on the host: only that header's include directory is
// added (see tests/CMakeLists.txt).

#include "wisp/port/esp/sntp_packet.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

using wisp::port::esp::kSntpPacketSize;
using wisp::port::esp::sntp_parse_reply;
using wisp::port::esp::sntp_request;
using wisp::port::esp::SntpPacket;

namespace {

constexpr uint64_t kNonce = 0x0123456789abcdefULL;
// 2026-01-01 00:00:00 UTC on the NTP timescale (seconds since 1900).
constexpr uint32_t kNtp2026 = 1767225600u + 2208988800u;

void put32(SntpPacket& p, size_t at, uint32_t v) {
    p[at] = static_cast<uint8_t>(v >> 24);
    p[at + 1] = static_cast<uint8_t>(v >> 16);
    p[at + 2] = static_cast<uint8_t>(v >> 8);
    p[at + 3] = static_cast<uint8_t>(v);
}

// A well-formed server reply to a request carrying kNonce.
SntpPacket reply(uint32_t seconds = kNtp2026, uint32_t fraction = 0) {
    SntpPacket p{};
    p[0] = (0 << 6) | (4 << 3) | 4;  // LI 0, version 4, mode 4 (server)
    p[1] = 2;                        // stratum
    put32(p, 24, static_cast<uint32_t>(kNonce >> 32));
    put32(p, 28, static_cast<uint32_t>(kNonce));
    put32(p, 40, seconds);
    put32(p, 44, fraction);
    return p;
}

}  // namespace

TEST(SntpPacket, RequestIsAVersion4ClientPacketCarryingTheNonce) {
    SntpPacket p;
    p.fill(0xff);
    sntp_request(p, kNonce);
    EXPECT_EQ(p[0], 0x23);  // LI 0, version 4, mode 3 (client)
    for (size_t i = 1; i < 40; ++i) EXPECT_EQ(p[i], 0) << i;
    const std::array<uint8_t, 8> nonce = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    for (size_t i = 0; i < 8; ++i) EXPECT_EQ(p[40 + i], nonce[i]) << i;
}

TEST(SntpPacket, ReadsTheTransmitTime) {
    SntpPacket p = reply();
    EXPECT_EQ(sntp_parse_reply(p.data(), p.size(), kNonce), 1767225600);
    // Half a second or more rounds up.
    p = reply(kNtp2026, 0x80000000u);
    EXPECT_EQ(sntp_parse_reply(p.data(), p.size(), kNonce), 1767225601);
    p = reply(kNtp2026, 0x7fffffffu);
    EXPECT_EQ(sntp_parse_reply(p.data(), p.size(), kNonce), 1767225600);
    // Version 3 servers answer too; a longer datagram (extensions) is fine.
    p = reply();
    p[0] = (3 << 3) | 4;
    EXPECT_EQ(sntp_parse_reply(p.data(), p.size(), kNonce), 1767225600);
    std::array<uint8_t, kSntpPacketSize + 20> longer{};
    SntpPacket r = reply();
    std::copy(r.begin(), r.end(), longer.begin());
    EXPECT_EQ(sntp_parse_reply(longer.data(), longer.size(), kNonce), 1767225600);
}

// Seconds with the top bit clear are past 2036-02-07 (NTP era 1).
TEST(SntpPacket, HandlesTheNextEra) {
    SntpPacket p = reply(0x00000010u);
    EXPECT_EQ(sntp_parse_reply(p.data(), p.size(), kNonce),
              int64_t{0x100000000LL} + 0x10 - 2208988800LL);
}

TEST(SntpPacket, RejectsWhatIsNotAnAnswerToUs) {
    SntpPacket ok = reply();
    EXPECT_FALSE(sntp_parse_reply(ok.data(), kSntpPacketSize - 1, kNonce));
    EXPECT_FALSE(sntp_parse_reply(ok.data(), ok.size(), kNonce + 1));  // not our request

    SntpPacket p = reply();
    p[0] = (4 << 3) | 3;  // a client packet
    EXPECT_FALSE(sntp_parse_reply(p.data(), p.size(), kNonce));
    p = reply();
    p[0] = (3 << 6) | (4 << 3) | 4;  // LI 3: server clock unsynchronized
    EXPECT_FALSE(sntp_parse_reply(p.data(), p.size(), kNonce));
    p = reply();
    p[0] = (2 << 3) | 4;  // version 2
    EXPECT_FALSE(sntp_parse_reply(p.data(), p.size(), kNonce));
    p = reply();
    p[1] = 0;  // kiss-o'-death
    EXPECT_FALSE(sntp_parse_reply(p.data(), p.size(), kNonce));
    p = reply();
    p[1] = 16;  // unsynchronized
    EXPECT_FALSE(sntp_parse_reply(p.data(), p.size(), kNonce));
    p = reply(0, 0);  // no transmit time
    EXPECT_FALSE(sntp_parse_reply(p.data(), p.size(), kNonce));
}
