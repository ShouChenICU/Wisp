// Unit tests for the captive-portal DNS answer builder used by
// wisp::port::esp's hotspot DNS responder (see
// wisp/port/esp/dns_reply.hpp). Compiles on the host without any ESP-IDF
// or lwIP include.

#include "wisp/port/esp/dns_reply.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using wisp::port::esp::captive_dns_reply;
using wisp::port::esp::kDnsMaxMessage;

namespace {

constexpr uint32_t kIp = 0xC0A80401;  // 192.168.4.1

constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypeAaaa = 28;
constexpr uint16_t kTypeAny = 255;
constexpr uint16_t kClassIn = 1;

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

uint16_t get16(const std::vector<uint8_t>& in, size_t at) {
    return static_cast<uint16_t>((in[at] << 8) | in[at + 1]);
}

void put_name(std::vector<uint8_t>& out, const std::string& dotted) {
    size_t start = 0;
    while (start < dotted.size()) {
        size_t dot = dotted.find('.', start);
        if (dot == std::string::npos) dot = dotted.size();
        out.push_back(static_cast<uint8_t>(dot - start));
        out.insert(out.end(), dotted.begin() + start, dotted.begin() + dot);
        start = dot + 1;
    }
    out.push_back(0);
}

std::vector<uint8_t> query(const std::string& name, uint16_t type = kTypeA, uint16_t cls = kClassIn,
                           uint16_t flags = 0x0100, uint16_t id = 0xBEEF) {
    std::vector<uint8_t> q;
    put16(q, id);
    put16(q, flags);
    put16(q, 1);  // QDCOUNT
    put16(q, 0);
    put16(q, 0);
    put16(q, 0);
    put_name(q, name);
    put16(q, type);
    put16(q, cls);
    return q;
}

std::vector<uint8_t> reply(const std::vector<uint8_t>& q, size_t cap = kDnsMaxMessage) {
    std::vector<uint8_t> out(cap);
    size_t n = captive_dns_reply(q.data(), q.size(), kIp, out.data(), out.size());
    out.resize(n);
    return out;
}

}  // namespace

TEST(DnsReply, AnswersAQueryWithTheHotspotAddress) {
    const auto q = query("connectivitycheck.gstatic.com");
    const auto r = reply(q);
    ASSERT_EQ(r.size(), q.size() + 16);
    EXPECT_EQ(get16(r, 0), 0xBEEF);
    const uint16_t flags = get16(r, 2);
    EXPECT_TRUE(flags & 0x8000);   // QR: a response
    EXPECT_TRUE(flags & 0x0400);   // AA
    EXPECT_TRUE(flags & 0x0100);   // RD copied from the query
    EXPECT_EQ(flags & 0x000f, 0);  // NOERROR
    EXPECT_EQ(get16(r, 4), 1);     // QDCOUNT
    EXPECT_EQ(get16(r, 6), 1);     // ANCOUNT
    EXPECT_EQ(get16(r, 8), 0);     // NSCOUNT
    EXPECT_EQ(get16(r, 10), 0);    // ARCOUNT
    // The question is echoed unchanged.
    EXPECT_TRUE(std::equal(q.begin() + 12, q.end(), r.begin() + 12));
    const size_t a = q.size();
    EXPECT_EQ(get16(r, a), 0xC00C);  // name: pointer to the question's
    EXPECT_EQ(get16(r, a + 2), kTypeA);
    EXPECT_EQ(get16(r, a + 4), kClassIn);
    EXPECT_GT((get16(r, a + 6) << 16) | get16(r, a + 8), 0);  // TTL
    EXPECT_EQ(get16(r, a + 10), 4);
    EXPECT_EQ(r[a + 12], 192);
    EXPECT_EQ(r[a + 13], 168);
    EXPECT_EQ(r[a + 14], 4);
    EXPECT_EQ(r[a + 15], 1);
}

TEST(DnsReply, AnyQueryAlsoGetsTheAddress) {
    const auto r = reply(query("captive.apple.com", kTypeAny));
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(get16(r, 6), 1);
}

// No IPv6 address to give: an empty NOERROR answer makes the phone fall
// back to its A query instead of waiting for a timeout.
TEST(DnsReply, AaaaGetsAnEmptyAnswer) {
    const auto q = query("captive.apple.com", kTypeAaaa);
    const auto r = reply(q);
    ASSERT_EQ(r.size(), q.size());
    EXPECT_TRUE(get16(r, 2) & 0x8000);
    EXPECT_EQ(get16(r, 2) & 0x000f, 0);
    EXPECT_EQ(get16(r, 6), 0);
}

TEST(DnsReply, RecursionDesiredIsOnlyEchoed) {
    const auto r = reply(query("a.b", kTypeA, kClassIn, /*flags=*/0));
    ASSERT_FALSE(r.empty());
    EXPECT_FALSE(get16(r, 2) & 0x0100);
}

// An EDNS OPT record in the additional section is ignored, not echoed.
TEST(DnsReply, AdditionalRecordsAreDropped) {
    auto q = query("example.com");
    q[11] = 1;  // ARCOUNT
    q.insert(q.end(), {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0});
    const auto r = reply(q);
    ASSERT_EQ(r.size(), q.size() - 11 + 16);
    EXPECT_EQ(get16(r, 10), 0);
}

TEST(DnsReply, RootNameIsAnswered) {
    const auto r = reply(query(""));
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(get16(r, 6), 1);
}

TEST(DnsReply, IgnoresResponses) {
    EXPECT_TRUE(reply(query("x.com", kTypeA, kClassIn, 0x8100)).empty());
}

TEST(DnsReply, IgnoresOtherOpcodes) {
    EXPECT_TRUE(reply(query("x.com", kTypeA, kClassIn, 0x2800)).empty());  // UPDATE
}

TEST(DnsReply, IgnoresMalformedQueries) {
    // Shorter than a header.
    EXPECT_TRUE(reply(std::vector<uint8_t>(11, 0)).empty());

    // No question, or more than one.
    auto none = query("x.com");
    none[5] = 0;
    EXPECT_TRUE(reply(none).empty());
    auto two = query("x.com");
    two[5] = 2;
    EXPECT_TRUE(reply(two).empty());

    // Name runs past the end of the packet.
    auto truncated = query("example.com");
    truncated.resize(16);
    EXPECT_TRUE(reply(truncated).empty());

    // Type/class missing.
    auto no_type = query("example.com");
    no_type.resize(no_type.size() - 2);
    EXPECT_TRUE(reply(no_type).empty());

    // A compression pointer has no business in a question.
    std::vector<uint8_t> pointer = query("");
    pointer[12] = 0xC0;
    pointer.insert(pointer.begin() + 13, 0x0C);
    EXPECT_TRUE(reply(pointer).empty());

    // Label longer than 63 bytes.
    EXPECT_TRUE(reply(query(std::string(64, 'a') + ".com")).empty());
}

TEST(DnsReply, RejectsNamesLongerThan255Bytes) {
    std::string name;
    for (int i = 0; i < 5; ++i) name += std::string(60, 'a') + ".";
    name += "com";
    EXPECT_TRUE(reply(query(name)).empty());

    std::string fits;
    for (int i = 0; i < 4; ++i) fits += std::string(60, 'a') + ".";
    fits += "com";
    EXPECT_FALSE(reply(query(fits)).empty());
}

TEST(DnsReply, NeedsRoomForTheWholeAnswer) {
    const auto q = query("example.com");
    EXPECT_TRUE(reply(q, q.size() + 15).empty());
    EXPECT_FALSE(reply(q, q.size() + 16).empty());
}
