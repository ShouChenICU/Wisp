#pragma once

// Pure, platform-free DNS answer builder for the setup hotspot's captive
// DNS responder (net/captive_dns.cpp): every name resolves to the
// hotspot's own address, which is what makes phones notice the captive
// portal (connectivity spec 4.3). Header-only and free of IDF/lwIP
// includes so it is unit-tested on the host
// (tests/port_esp/dns_reply_test.cpp).
//
// Only plain standard queries with exactly one question are answered;
// anything else (responses, other opcodes, truncated or malformed
// packets, compression pointers in the question) gets no reply at all,
// so a stray packet cannot make the responder emit garbage. An A or ANY
// question of class IN gets one A record; any other question (notably
// AAAA) gets an empty NOERROR answer, so the client falls back to its A
// query at once instead of timing out. Additional records in the query
// (an EDNS OPT) are ignored and not echoed.

#include <cstddef>
#include <cstdint>

namespace wisp::port::esp {

// Classic DNS-over-UDP message limit (RFC 1035 section 4.2.1).
inline constexpr size_t kDnsMaxMessage = 512;
// Short, so a phone that leaves the hotspot soon forgets the fake answers.
inline constexpr uint32_t kCaptiveDnsTtlSeconds = 60;

// Writes the reply to `query` (`len` bytes) into `out` (`cap` bytes) and
// returns its length, or 0 when the query is to be ignored or the reply
// does not fit. `ip` is the address to answer with, in host byte order.
inline size_t captive_dns_reply(const uint8_t* query, size_t len, uint32_t ip, uint8_t* out,
                                size_t cap) {
    constexpr size_t kHeader = 12;
    constexpr size_t kAnswer = 16;  // pointer, type, class, TTL, length, address
    constexpr size_t kMaxName = 255;
    if (query == nullptr || out == nullptr || len < kHeader) return 0;

    const auto get16 = [query](size_t at) {
        return static_cast<uint16_t>((query[at] << 8) | query[at + 1]);
    };
    const uint16_t flags = get16(2);
    const bool is_response = (flags & 0x8000) != 0;
    const unsigned opcode = (flags >> 11) & 0x0f;
    if (is_response || opcode != 0 || get16(4) != 1) return 0;

    // QNAME: length-prefixed labels up to a zero byte.
    size_t pos = kHeader;
    size_t name_len = 0;
    for (;;) {
        if (pos >= len) return 0;
        const uint8_t label = query[pos];
        if (label > 63) return 0;  // compression pointer or reserved bits
        name_len += label + 1u;
        if (name_len > kMaxName) return 0;
        pos += label + 1u;
        if (label == 0) break;
    }
    if (pos + 4 > len) return 0;
    const uint16_t qtype = get16(pos);
    const uint16_t qclass = get16(pos + 2);
    const size_t question_end = pos + 4;

    const bool answer = qclass == 1 && (qtype == 1 || qtype == 255);
    const size_t total = question_end + (answer ? kAnswer : 0);
    if (total > cap) return 0;

    for (size_t i = 0; i < question_end; ++i) out[i] = query[i];
    // QR + AA, opcode QUERY, RD echoed, RCODE NOERROR.
    const uint16_t reply_flags = static_cast<uint16_t>(0x8400 | (flags & 0x0100));
    const auto put16 = [out](size_t at, uint16_t v) {
        out[at] = static_cast<uint8_t>(v >> 8);
        out[at + 1] = static_cast<uint8_t>(v & 0xff);
    };
    put16(2, reply_flags);
    put16(4, 1);
    put16(6, answer ? 1 : 0);
    put16(8, 0);
    put16(10, 0);
    if (answer) {
        size_t a = question_end;
        put16(a, 0xC00C);  // the name: a pointer to the question's
        put16(a + 2, 1);
        put16(a + 4, 1);
        put16(a + 6, static_cast<uint16_t>(kCaptiveDnsTtlSeconds >> 16));
        put16(a + 8, static_cast<uint16_t>(kCaptiveDnsTtlSeconds & 0xffff));
        put16(a + 10, 4);
        put16(a + 12, static_cast<uint16_t>(ip >> 16));
        put16(a + 14, static_cast<uint16_t>(ip & 0xffff));
    }
    return total;
}

}  // namespace wisp::port::esp
