#include "wisp/net/pairing_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/base64.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/sha256.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::core::DeviceRegistry;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::StorageManager;
using wisp::net::ClientInfo;
using wisp::net::Iface;
using wisp::net::PairedClient;
using wisp::net::PairingConfig;
using wisp::net::PairingService;
using wisp::test::FakeStorage;

namespace {

constexpr const char* kChromeAndroid =
    "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Mobile Safari/537.36";
constexpr const char* kSafariIphone =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like "
    "Gecko) Version/18.0 Mobile/15E148 Safari/604.1";
constexpr const char* kFirefoxWindows =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:131.0) Gecko/20100101 Firefox/131.0";
constexpr const char* kEdgeWindows =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Safari/537.36 Edg/130.0.0.0";
constexpr const char* kChromeMac =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Safari/537.36";

ClientInfo browser(Iface iface = Iface::Sta, std::string ua = kChromeAndroid) {
    return ClientInfo{std::move(ua), "192.168.1.23", iface};
}

// A started bus collecting every "net."-prefixed event, a StorageManager
// over a temp dir, a seeded (or scripted) random source and a settable
// wall clock. Tests advance monotonic time by passing explicit now_ms.
struct Fixture {
    Fixture() : dir(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        bus.start();
        bus.subscribe("net.", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });
    }

    ~Fixture() {
        svc.reset();
        bus.stop();
    }

    // (Re)creates the service over the same storage, as a reboot would.
    PairingService& make(PairingConfig config = {}) {
        svc.reset();
        svc = std::make_unique<PairingService>(
            *storage, bus,
            [this](uint8_t* out, size_t len) {
                std::lock_guard<std::mutex> lock(mutex);
                for (size_t i = 0; i < len; ++i) {
                    if (!scripted_random.empty()) {
                        out[i] = scripted_random.front();
                        scripted_random.erase(scripted_random.begin());
                    } else {
                        out[i] = static_cast<uint8_t>(rng());
                    }
                }
            },
            [this] { return unix_now.load(); }, config);
        EXPECT_TRUE(svc->init());
        return *svc;
    }

    std::string clients_path() const { return dir + "/system/net/clients.json"; }

    std::string read_clients() {
        std::FILE* f = std::fopen(clients_path().c_str(), "rb");
        if (f == nullptr) return {};
        std::string out;
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        return out;
    }

    // Requests a code and confirms it; returns the token.
    std::string pair(uint64_t now_ms, ClientInfo client = browser()) {
        EXPECT_TRUE(svc->request_code(now_ms));
        auto code = svc->active_code(now_ms);
        EXPECT_TRUE(code.has_value());
        auto token = svc->confirm(code.value_or(""), client, now_ms);
        EXPECT_TRUE(token) << (token ? "" : token.error().message);
        return token ? token.value() : std::string();
    }

    std::vector<Event> topic(const std::string& t) {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Event> out;
        for (const auto& e : events) {
            if (e.topic == t) out.push_back(e);
        }
        return out;
    }

    std::vector<Event> all_events() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }

    std::string dir;
    DeviceRegistry registry;
    EventBus bus;
    std::unique_ptr<StorageManager> storage;
    std::unique_ptr<PairingService> svc;
    std::atomic<int64_t> unix_now{1'700'000'000};

    std::mutex mutex;
    std::vector<Event> events;
    std::mt19937 rng{4242};
    std::vector<uint8_t> scripted_random;
};

std::string wrong_code(const std::string& code) { return code == "000000" ? "000001" : "000000"; }

std::string hash_hex(const std::string& token) {
    auto digest = wisp::sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(token.data()), token.size()));
    return wisp::to_hex(digest);
}

// Whether `secret` occurs in `hay`. A 6-digit code is only matched as a
// whole number: log lines and event payloads carry timestamps and other
// numbers that contain any given digit run by chance.
bool leaks(const std::string& hay, const std::string& secret) {
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    bool numeric = std::all_of(secret.begin(), secret.end(), is_digit);
    for (size_t pos = hay.find(secret); pos != std::string::npos; pos = hay.find(secret, pos + 1)) {
        if (!numeric) return true;
        size_t end = pos + secret.size();
        bool digit_before = pos > 0 && is_digit(hay[pos - 1]);
        bool digit_after = end < hay.size() && is_digit(hay[end]);
        if (!digit_before && !digit_after) return true;
    }
    return false;
}

std::optional<PairedClient> find_client(const PairingService& svc, const std::string& id) {
    for (const auto& c : svc.clients()) {
        if (c.id == id) return c;
    }
    return std::nullopt;
}

}  // namespace

TEST(Pairing, CodeIsSixDigitsAndExpires) {
    Fixture f;
    auto& svc = f.make();
    EXPECT_FALSE(svc.active_code(0).has_value());

    ASSERT_TRUE(svc.request_code(1000));
    auto code = svc.active_code(1000);
    ASSERT_TRUE(code.has_value());
    EXPECT_TRUE(std::regex_match(*code, std::regex("[0-9]{6}"))) << *code;
    EXPECT_TRUE(svc.active_code(1000 + 119999).has_value());
    EXPECT_FALSE(svc.active_code(1000 + 120000).has_value());
    auto late = svc.confirm(*code, browser(), 1000 + 120000);
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code, ErrorCode::PermissionDenied);

    // Expiry is announced once the service observes it.
    svc.tick(1000 + 120000);
    auto evs = f.topic("net.pairing.code");
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_TRUE(evs[0].payload.at("active").as_bool());
    EXPECT_EQ(evs[0].payload.at("expires_ms").as_int(), 1000 + 120000);
    EXPECT_FALSE(evs[1].payload.at("active").as_bool());

    // Leading zeros are kept.
    f.scripted_random = {0, 0, 0, 0};
    ASSERT_TRUE(svc.request_code(200000));
    EXPECT_EQ(svc.active_code(200000).value_or(""), "000000");
}

TEST(Pairing, NewRequestInvalidatesOldCode) {
    Fixture f;
    auto& svc = f.make();
    f.scripted_random = {0, 0, 0, 0};
    ASSERT_TRUE(svc.request_code(1000));
    std::string first = svc.active_code(1000).value_or("");
    ASSERT_EQ(first, "000000");

    f.scripted_random = {0, 0, 0, 7};
    ASSERT_TRUE(svc.request_code(2000));
    std::string second = svc.active_code(2000).value_or("");
    ASSERT_EQ(second.size(), 6u);
    ASSERT_NE(second, first);

    auto old = svc.confirm(first, browser(), 3000);
    ASSERT_FALSE(old);
    EXPECT_EQ(old.error().code, ErrorCode::PermissionDenied);
    EXPECT_TRUE(svc.confirm(second, browser(), 3000));
    // A code works only once.
    EXPECT_FALSE(svc.active_code(3000).has_value());
    EXPECT_FALSE(svc.confirm(second, browser(), 3000));

    auto evs = f.topic("net.pairing.code");
    ASSERT_FALSE(evs.empty());
    EXPECT_FALSE(evs.back().payload.at("active").as_bool());
}

TEST(Pairing, LocksAfterFiveWrongCodes) {
    Fixture f;
    auto& svc = f.make();
    ASSERT_TRUE(svc.request_code(1000));
    std::string code = svc.active_code(1000).value_or("");
    std::string bad = wrong_code(code);

    for (int i = 1; i <= 4; ++i) {
        auto r = svc.confirm(i == 2 ? "12ab" : bad, browser(), 1000 + i);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::PermissionDenied);
        EXPECT_EQ(svc.active_code(1000 + i).value_or(""), code);  // still valid
    }
    auto fifth = svc.confirm(bad, browser(), 1005);
    ASSERT_FALSE(fifth);
    EXPECT_EQ(fifth.error().code, ErrorCode::PermissionDenied);

    // Locked: even the right code is refused, and no new code is issued.
    EXPECT_FALSE(svc.active_code(1006).has_value());
    auto locked = svc.confirm(code, browser(), 1006);
    ASSERT_FALSE(locked);
    EXPECT_EQ(locked.error().code, ErrorCode::RateLimited);
    auto req = svc.request_code(1005 + 299999);
    ASSERT_FALSE(req);
    EXPECT_EQ(req.error().code, ErrorCode::RateLimited);

    auto failed = f.topic("net.pairing.failed");
    ASSERT_EQ(failed.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(failed[i].payload.at("attempts").as_int(), i + 1);
        EXPECT_EQ(failed[i].payload.at("locked").as_bool(), i == 4);
    }

    // The lock lifts after 5 minutes, with a fresh attempt budget.
    ASSERT_TRUE(svc.request_code(1005 + 300000));
    std::string fresh = svc.active_code(1005 + 300000).value_or("");
    auto again = svc.confirm(wrong_code(fresh), browser(), 1005 + 300001);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(f.topic("net.pairing.failed").back().payload.at("attempts").as_int(), 1);
    EXPECT_TRUE(svc.confirm(fresh, browser(), 1005 + 300002));
}

TEST(Pairing, TokenStoredOnlyAsHash) {
    Fixture f;
    auto& svc = f.make();
    std::string token = f.pair(1000);
    ASSERT_EQ(token.size(), 43u);  // base64url of 32 bytes, unpadded
    EXPECT_TRUE(std::regex_match(token, std::regex("[A-Za-z0-9_-]{43}"))) << token;

    std::string hex = hash_hex(token);
    std::string raw = f.read_clients();
    ASSERT_FALSE(raw.empty());
    EXPECT_EQ(raw.find(token), std::string::npos) << raw;
    EXPECT_NE(raw.find(hex), std::string::npos) << raw;

    auto clients = svc.clients();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients[0].id, hex.substr(0, 12));
    EXPECT_EQ(clients[0].label, "Chrome · Android");
    EXPECT_EQ(clients[0].created_unix, 1'700'000'000);
    EXPECT_EQ(clients[0].last_seen_unix, 1'700'000'000);

    auto doc = wisp::from_json(raw);
    ASSERT_TRUE(doc);
    const wisp::Value* list = doc.value().find("clients");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->as_list().size(), 1u);
    const auto& entry = list->as_list()[0].as_map();
    EXPECT_EQ(entry.size(), 5u);  // id, hash, label, created, last_seen -- nothing else
    EXPECT_EQ(entry.at("hash").as_string(), hex);
    EXPECT_EQ(entry.at("id").as_string(), hex.substr(0, 12));

    auto id = svc.authenticate(token, 2000);
    ASSERT_TRUE(id);
    EXPECT_EQ(id.value(), hex.substr(0, 12));
}

TEST(Pairing, LabelSummarisesUserAgent) {
    Fixture f;
    auto& svc = f.make();
    struct Case {
        const char* ua;
        const char* label;
    };
    const Case cases[] = {
        {kChromeAndroid, "Chrome · Android"},
        {kSafariIphone, "Safari · iPhone"},
        {kFirefoxWindows, "Firefox · Windows"},
        {kEdgeWindows, "Edge · Windows"},
        {kChromeMac, "Chrome · macOS"},
        {"curl/8.5.0", "浏览器"},
        {"", "浏览器"},
    };
    uint64_t now = 1000;
    for (const auto& c : cases) {
        auto token = svc.pair_from_ap(browser(Iface::Ap, c.ua), now++);
        ASSERT_TRUE(token);
        auto id = svc.authenticate(token.value(), now);
        ASSERT_TRUE(id);
        EXPECT_EQ(find_client(svc, id.value())->label, c.label) << c.ua;
    }
}

TEST(Pairing, AuthenticateSurvivesRestart) {
    Fixture f;
    f.make();
    std::string token = f.pair(1000, browser(Iface::Sta, kFirefoxWindows));
    std::string id = hash_hex(token).substr(0, 12);

    f.unix_now = 1'700'000'500;
    auto& svc = f.make();  // a fresh instance reading clients.json
    auto got = svc.authenticate(token, 10);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value(), id);
    auto clients = svc.clients();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients[0].label, "Firefox · Windows");
    EXPECT_EQ(clients[0].created_unix, 1'700'000'000);
    EXPECT_EQ(clients[0].last_seen_unix, 1'700'000'500);

    for (const char* bogus : {"", "nope", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}) {
        auto r = svc.authenticate(bogus, 20);
        ASSERT_FALSE(r) << bogus;
        EXPECT_EQ(r.error().code, ErrorCode::PermissionDenied);
    }
}

TEST(Pairing, StartsEmptyOnCorruptFile) {
    Fixture f;
    std::string junk = "{not json";
    ASSERT_TRUE(
        f.storage->write_atomic(f.clients_path(), std::vector<uint8_t>(junk.begin(), junk.end())));
    auto svc = std::make_unique<PairingService>(
        *f.storage, f.bus, [](uint8_t* out, size_t len) { std::fill(out, out + len, 1); },
        [] { return int64_t{0}; });
    EXPECT_FALSE(svc->init());
    EXPECT_TRUE(svc->clients().empty());
    // Still usable.
    EXPECT_TRUE(svc->pair_from_ap(browser(Iface::Ap), 0));
}

TEST(Pairing, EvictsLeastRecentlySeenNinthClient) {
    Fixture f;
    auto& svc = f.make();
    std::vector<std::string> tokens;
    for (int i = 0; i < 8; ++i) {
        f.unix_now = 100 + i;  // created == last_seen == 100 + i
        tokens.push_back(f.pair(1000 + i));
    }
    ASSERT_EQ(svc.clients().size(), 8u);

    f.unix_now = 200;
    ASSERT_TRUE(svc.authenticate(tokens[0], 2000));  // client 0 is now the most recent
    f.unix_now = 102;
    ASSERT_TRUE(svc.authenticate(tokens[1], 2001));  // ties client 2 (102) but is older

    f.unix_now = 300;
    std::string ninth = f.pair(3000);
    ASSERT_EQ(svc.clients().size(), 8u);

    auto evicted = svc.authenticate(tokens[1], 3001);
    ASSERT_FALSE(evicted);
    EXPECT_EQ(evicted.error().code, ErrorCode::PermissionDenied);
    EXPECT_FALSE(find_client(svc, hash_hex(tokens[1]).substr(0, 12)).has_value());
    for (int i : {0, 2, 3, 4, 5, 6, 7}) {
        EXPECT_TRUE(svc.authenticate(tokens[i], 3002)) << i;
    }
    EXPECT_TRUE(svc.authenticate(ninth, 3003));
    EXPECT_EQ(f.read_clients().find(hash_hex(tokens[1])), std::string::npos);
}

TEST(Pairing, ApPairingRequiresApInterface) {
    Fixture f;
    auto& svc = f.make();
    for (Iface iface : {Iface::Sta, Iface::Loopback}) {
        auto r = svc.pair_from_ap(browser(iface), 1000);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::PermissionDenied);
    }
    EXPECT_TRUE(svc.clients().empty());

    auto token = svc.pair_from_ap(browser(Iface::Ap), 1000);
    ASSERT_TRUE(token);
    auto id = svc.authenticate(token.value(), 1001);
    ASSERT_TRUE(id);
    EXPECT_EQ(id.value(), hash_hex(token.value()).substr(0, 12));
}

TEST(Pairing, LastSeenFlushIsThrottled) {
    Fixture f;
    auto& svc = f.make();
    const uint64_t t0 = 1000;
    std::string token = f.pair(t0);  // adding a client persists immediately
    std::string id = hash_hex(token).substr(0, 12);
    std::string before = f.read_clients();
    ASSERT_NE(before.find(id), std::string::npos);

    // Every authenticate() moves last_seen forward, so any write would
    // change the file's bytes: unchanged bytes mean no write happened.
    int writes = 0;
    std::string last = before;
    auto check = [&] {
        std::string now = f.read_clients();
        if (now != last) {
            ++writes;
            last = now;
        }
    };
    for (int i = 1; i <= 100; ++i) {
        ++f.unix_now;
        uint64_t now = i == 100 ? t0 + 3599999 : t0 + static_cast<uint64_t>(i) * 30000;
        ASSERT_TRUE(svc.authenticate(token, now));
        check();
        EXPECT_EQ(find_client(svc, id)->last_seen_unix, f.unix_now.load());  // memory is current
    }
    EXPECT_EQ(writes, 0);

    ++f.unix_now;
    ASSERT_TRUE(svc.authenticate(token, t0 + 3600000));
    check();
    EXPECT_EQ(writes, 1);
    auto doc = wisp::from_json(last);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc.value().find("clients")->as_list()[0].find("last_seen")->as_int(),
              f.unix_now.load());

    // The flush restarted the interval.
    ++f.unix_now;
    ASSERT_TRUE(svc.authenticate(token, t0 + 3600001));
    check();
    EXPECT_EQ(writes, 1);

    // Pairing and revoking still write at once, carrying the newest
    // last_seen along.
    std::string other = f.pair(t0 + 3600002);
    check();
    EXPECT_EQ(writes, 2);
    EXPECT_NE(last.find(hash_hex(other)), std::string::npos);
    ASSERT_TRUE(svc.revoke(hash_hex(other).substr(0, 12)));
    check();
    EXPECT_EQ(writes, 3);
    EXPECT_EQ(last.find(hash_hex(other)), std::string::npos);
}

TEST(Pairing, RevokeAllClearsFile) {
    Fixture f;
    auto& svc = f.make();
    std::string a = f.pair(1000);
    std::string b = f.pair(2000);
    ASSERT_EQ(svc.clients().size(), 2u);

    auto missing = svc.revoke("000000000000");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    ASSERT_TRUE(svc.revoke(hash_hex(a).substr(0, 12)));
    EXPECT_FALSE(svc.authenticate(a, 3000));
    EXPECT_TRUE(svc.authenticate(b, 3000));

    svc.revoke_all();
    EXPECT_TRUE(svc.clients().empty());
    EXPECT_FALSE(svc.authenticate(b, 4000));
    std::string raw = f.read_clients();
    EXPECT_EQ(raw.find(hash_hex(b)), std::string::npos) << raw;
    auto doc = wisp::from_json(raw);
    ASSERT_TRUE(doc);
    ASSERT_NE(doc.value().find("clients"), nullptr);
    EXPECT_TRUE(doc.value().find("clients")->as_list().empty());

    auto& again = f.make();
    EXPECT_TRUE(again.clients().empty());
}

TEST(Pairing, NewCodeDoesNotResetFailureCount) {
    // /pair/start needs no authentication, so a fresh code must not hand
    // a guesser a fresh attempt budget.
    Fixture f;
    auto& svc = f.make();
    ASSERT_TRUE(svc.request_code(1000));
    std::string code = svc.active_code(1000).value_or("");
    for (int i = 1; i <= 4; ++i) {
        EXPECT_FALSE(svc.confirm(wrong_code(code), browser(), 1000 + i));
    }
    ASSERT_TRUE(svc.request_code(2000));
    std::string fresh = svc.active_code(2000).value_or("");
    auto fifth = svc.confirm(wrong_code(fresh), browser(), 2001);
    ASSERT_FALSE(fifth);
    EXPECT_EQ(fifth.error().code, ErrorCode::PermissionDenied);

    auto failed = f.topic("net.pairing.failed");
    ASSERT_EQ(failed.size(), 5u);
    EXPECT_EQ(failed.back().payload.at("attempts").as_int(), 5);
    EXPECT_TRUE(failed.back().payload.at("locked").as_bool());
    auto locked = svc.confirm(fresh, browser(), 2002);
    ASSERT_FALSE(locked);
    EXPECT_EQ(locked.error().code, ErrorCode::RateLimited);
}

TEST(Pairing, ConfirmWithoutActiveCodeCountsAsFailure) {
    Fixture f;
    auto& svc = f.make();
    auto r = svc.confirm("123456", browser(), 1000);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::PermissionDenied);
    auto failed = f.topic("net.pairing.failed");
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0].payload.at("attempts").as_int(), 1);
    EXPECT_FALSE(failed[0].payload.at("locked").as_bool());

    for (int i = 1; i <= 4; ++i) {
        EXPECT_FALSE(svc.confirm("", browser(), 1000 + i));
    }
    EXPECT_TRUE(f.topic("net.pairing.failed").back().payload.at("locked").as_bool());
    auto req = svc.request_code(1010);
    ASSERT_FALSE(req);
    EXPECT_EQ(req.error().code, ErrorCode::RateLimited);
}

TEST(Pairing, RevokeAllRemovesFileWhenRewriteFails) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    {
        Fixture f;
        auto& svc = f.make();
        std::string token = f.pair(1000);
        ASSERT_NE(f.read_clients().find(hash_hex(token)), std::string::npos);

        // A non-empty directory where write_atomic() puts its temp file
        // makes the rewrite fail while leaving the old file in place.
        std::filesystem::create_directories(f.clients_path() + ".tmp/blocker");

        svc.revoke_all();
        EXPECT_TRUE(svc.clients().empty());
        EXPECT_FALSE(std::filesystem::exists(f.clients_path()));

        // Nothing revives after a reboot.
        std::filesystem::remove_all(f.clients_path() + ".tmp");
        auto& again = f.make();
        EXPECT_TRUE(again.clients().empty());
        EXPECT_FALSE(again.authenticate(token, 2000));

        std::string log = wisp::log::ring_snapshot();
        EXPECT_NE(log.find("not persisted"), std::string::npos) << log;
    }
    wisp::log::reset_for_test();
}

TEST(Pairing, CodeNeverAppearsInEventsOrLog) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    {
        Fixture f;
        auto& svc = f.make();
        std::vector<std::string> secrets;

        ASSERT_TRUE(svc.request_code(1000));
        secrets.push_back(svc.active_code(1000).value_or(""));
        ASSERT_TRUE(svc.request_code(2000));  // replaces the first code
        std::string code = svc.active_code(2000).value_or("");
        secrets.push_back(code);
        for (int i = 0; i < 5; ++i) {
            EXPECT_FALSE(svc.confirm(wrong_code(code), browser(), 3000 + i));
        }
        ASSERT_TRUE(svc.request_code(400000));
        code = svc.active_code(400000).value_or("");
        secrets.push_back(code);
        auto token = svc.confirm(code, browser(), 400001);
        ASSERT_TRUE(token);
        secrets.push_back(token.value());
        ASSERT_TRUE(svc.request_code(500000));
        secrets.push_back(svc.active_code(500000).value_or(""));
        svc.tick(500000 + 120000);  // expires
        auto ap = svc.pair_from_ap(browser(Iface::Ap), 700000);
        ASSERT_TRUE(ap);
        secrets.push_back(ap.value());
        ASSERT_TRUE(svc.authenticate(ap.value(), 700001));

        auto events = f.all_events();
        ASSERT_FALSE(events.empty());
        std::string log = wisp::log::ring_snapshot();
        EXPECT_FALSE(log.empty());
        for (const auto& secret : secrets) {
            ASSERT_FALSE(secret.empty());
            EXPECT_FALSE(leaks(log, secret)) << secret << "\n" << log;
            for (const auto& e : events) {
                std::string json = wisp::to_json(wisp::Value(e.payload));
                EXPECT_FALSE(leaks(json, secret)) << e.topic << " " << json;
            }
        }
    }
    wisp::log::reset_for_test();
}

TEST(Pairing, ConcurrentAuthenticateAndRequestCode) {
    Fixture f;
    auto& svc = f.make();
    std::string token = f.pair(1000);
    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) {
                if (!svc.authenticate(token, 2000 + static_cast<uint64_t>(t * 1000 + i))) {
                    ok = false;
                }
                (void)svc.clients();
            }
        });
    }
    threads.emplace_back([&] {
        for (int i = 0; i < 200; ++i) {
            (void)svc.request_code(2000 + static_cast<uint64_t>(i));
            (void)svc.active_code(2000 + static_cast<uint64_t>(i));
        }
    });
    for (auto& th : threads) th.join();
    EXPECT_TRUE(ok.load());
}

// The admin backend closes a revoked browser's WebSocket through this hook
// (spec 6.4: revocation is the remedy for a captured token).
TEST(Pairing, RevokeListenersHearRevocations) {
    Fixture f;
    auto& svc = f.make();
    std::string a = f.pair(1000);
    f.pair(2000);
    const std::string a_id = hash_hex(a).substr(0, 12);

    std::vector<std::optional<std::string>> heard;
    std::vector<std::optional<std::string>> heard2;
    const int h1 = svc.add_revoke_listener([&](const std::optional<std::string>& id) {
        heard.push_back(id);
        // Runs outside the service's lock, so calling back in is fine.
        (void)svc.clients();
    });
    const int h2 = svc.add_revoke_listener(
        [&](const std::optional<std::string>& id) { heard2.push_back(id); });
    EXPECT_NE(h1, h2);

    ASSERT_FALSE(svc.revoke("000000000000"));
    EXPECT_TRUE(heard.empty());

    ASSERT_TRUE(svc.revoke(a_id));
    ASSERT_EQ(heard.size(), 1u);
    EXPECT_EQ(heard[0], std::optional<std::string>(a_id));

    svc.remove_revoke_listener(h2);
    svc.revoke_all();
    ASSERT_EQ(heard.size(), 2u);
    EXPECT_EQ(heard[1], std::nullopt);
    EXPECT_EQ(heard2.size(), 1u);
    svc.remove_revoke_listener(h1);
}

// Making room for a ninth browser revokes the least recently seen one, so
// its WebSocket must be closed just like after an explicit revoke().
TEST(Pairing, EvictionNotifiesRevokeListeners) {
    Fixture f;
    auto& svc = f.make();
    std::vector<std::string> tokens;
    for (int i = 0; i < 8; ++i) {
        f.unix_now = 100 + i;
        tokens.push_back(f.pair(1000 + i));
    }
    std::vector<std::optional<std::string>> heard;
    const int h = svc.add_revoke_listener([&](const std::optional<std::string>& id) {
        heard.push_back(id);
        // Outside the service's lock.
        (void)svc.clients();
    });

    f.unix_now = 300;
    f.pair(3000);
    ASSERT_EQ(heard.size(), 1u);
    EXPECT_EQ(heard[0], std::optional<std::string>(hash_hex(tokens[0]).substr(0, 12)));

    // The hotspot path evicts the same way.
    f.unix_now = 301;
    ASSERT_TRUE(svc.pair_from_ap(browser(Iface::Ap), 3001));
    ASSERT_EQ(heard.size(), 2u);
    EXPECT_EQ(heard[1], std::optional<std::string>(hash_hex(tokens[1]).substr(0, 12)));
    svc.remove_revoke_listener(h);
}
