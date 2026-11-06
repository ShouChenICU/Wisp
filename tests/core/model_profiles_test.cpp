#include "wisp/core/model_profiles.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::Map;
using wisp::Value;
using wisp::core::DeviceRegistry;
using wisp::core::EventBus;
using wisp::core::ModelFailure;
using wisp::core::ModelKind;
using wisp::core::ModelProbe;
using wisp::core::ModelProfile;
using wisp::core::ModelProfileService;
using wisp::core::ProfileInput;
using wisp::core::ProfileUpdate;
using wisp::core::ResolvedProfile;
using wisp::core::StorageManager;
using wisp::test::FakeStorage;

namespace {

struct Fixture {
    Fixture() : dir(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        bus.start();
        start();
    }

    ~Fixture() {
        svc.reset();
        bus.stop();
    }

    void start() {
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        svc = std::make_unique<ModelProfileService>(*storage, bus, [this](uint8_t* out, size_t n) {
            for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(++seed * 37);
        });
        EXPECT_TRUE(svc->init());
    }

    // Simulates a reboot: a fresh StorageManager and service over the same
    // directory.
    void restart() {
        svc.reset();
        start();
    }

    std::string models_path() const { return dir + "/system/secrets/models.json"; }
    std::string legacy_path() const { return dir + "/system/secrets/llm.json"; }

    static std::string read(const std::string& path) {
        std::string out;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return out;
        char buf[512];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        return out;
    }

    static void write(const std::string& path, const std::string& text) {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        FILE* f = std::fopen(path.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }

    // Creates a profile and returns it, failing the test on an error.
    ModelProfile make(ProfileInput in) {
        auto r = svc->create(in);
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? r.value() : ModelProfile{};
    }

    std::string key_of(const std::string& id) {
        // Only reachable through the active profile: activate, read, done.
        EXPECT_TRUE(svc->activate(id));
        auto kind = ModelKind::Llm;
        for (const ModelProfile& p : svc->get().profiles) {
            if (p.id == id) kind = p.kind;
        }
        auto r = svc->active_for_request(kind);
        return r ? r->api_key : std::string("<none>");
    }

    std::string dir;
    uint8_t seed = 0;
    EventBus bus;
    DeviceRegistry registry;
    std::unique_ptr<StorageManager> storage;
    std::unique_ptr<ModelProfileService> svc;
};

ProfileInput llm(std::string name, std::string base_url = "https://api.example.com/v1",
                 std::optional<std::string> key = std::nullopt) {
    return ProfileInput{.kind = ModelKind::Llm,
                        .name = std::move(name),
                        .preset = "custom",
                        .base_url = std::move(base_url),
                        .api_key = std::move(key)};
}

const ModelProfile* find(const wisp::core::ProfilesState& s, const std::string& id) {
    for (const ModelProfile& p : s.profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

TEST(ModelProfiles, PresetTableMatchesTheSpec) {
    auto presets = wisp::core::model_presets();
    std::vector<std::string> ids;
    for (const auto& p : presets) ids.emplace_back(p.id);
    EXPECT_EQ(ids, (std::vector<std::string>{"custom", "openai", "anthropic", "gemini", "deepseek",
                                             "qwen", "moonshot", "zhipu", "ark", "siliconflow",
                                             "groq", "openrouter", "ollama"}));
    const auto* custom = wisp::core::find_model_preset("custom");
    ASSERT_NE(custom, nullptr);
    EXPECT_TRUE(custom->base_url.empty());
    EXPECT_FALSE(custom->key_required);
    const auto* groq = wisp::core::find_model_preset("groq");
    ASSERT_NE(groq, nullptr);
    EXPECT_TRUE(groq->supports(ModelKind::Asr));
    EXPECT_FALSE(groq->supports(ModelKind::Tts));
    EXPECT_EQ(wisp::core::find_model_preset("nope"), nullptr);
}

TEST(ModelProfiles, CreateFillsPresetBaseUrlAndActivatesFirstOfKind) {
    Fixture fx;
    auto r = fx.svc->create({.kind = ModelKind::Llm, .name = "My DeepSeek", .preset = "deepseek"});
    ASSERT_TRUE(r) << r.error().message;
    const ModelProfile& p = r.value();
    EXPECT_EQ(p.id.size(), 8u);
    EXPECT_EQ(p.id.find_first_not_of("0123456789abcdef"), std::string::npos) << p.id;
    EXPECT_EQ(p.kind, ModelKind::Llm);
    EXPECT_EQ(p.name, "My DeepSeek");
    EXPECT_EQ(p.preset, "deepseek");
    EXPECT_EQ(p.base_url, "https://api.deepseek.com/v1");
    EXPECT_EQ(p.model, "");
    EXPECT_FALSE(p.has_key);

    auto s = fx.svc->get();
    EXPECT_EQ(s.active_for(ModelKind::Llm), p.id);
    EXPECT_EQ(s.active_for(ModelKind::Asr), "");
    EXPECT_EQ(s.active_for(ModelKind::Tts), "");

    // The second of a kind does not take over.
    ModelProfile second = fx.make(llm("Second"));
    EXPECT_NE(second.id, p.id);
    EXPECT_EQ(fx.svc->get().active_for(ModelKind::Llm), p.id);
    // An explicit base_url wins over the preset's.
    ModelProfile own = fx.make({.kind = ModelKind::Llm,
                                .name = "Proxy",
                                .preset = "openai",
                                .base_url = "https://proxy.example/v1/"});
    EXPECT_EQ(own.base_url, "https://proxy.example/v1");
}

TEST(ModelProfiles, ActiveIsPerKind) {
    Fixture fx;
    ModelProfile l1 = fx.make(llm("l1"));
    ModelProfile l2 = fx.make(llm("l2"));
    ModelProfile a1 = fx.make({.kind = ModelKind::Asr, .name = "a1", .preset = "openai"});
    ModelProfile t1 = fx.make({.kind = ModelKind::Tts,
                               .name = "t1",
                               .preset = "siliconflow",
                               .options = {{{"voice", "alex"}}}});
    ASSERT_TRUE(fx.svc->activate(l2.id));
    auto s = fx.svc->get();
    EXPECT_EQ(s.active_for(ModelKind::Llm), l2.id);
    EXPECT_EQ(s.active_for(ModelKind::Asr), a1.id);
    EXPECT_EQ(s.active_for(ModelKind::Tts), t1.id);
    auto r = fx.svc->activate("deadbeef");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(ModelProfiles, PresetMustSupportKind) {
    Fixture fx;
    auto r = fx.svc->create({.kind = ModelKind::Asr, .name = "x", .preset = "deepseek"});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    r = fx.svc->create({.kind = ModelKind::Llm, .name = "x", .preset = "unknown"});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);

    // Switching the preset later has to fit the profile's kind too.
    ModelProfile a = fx.make({.kind = ModelKind::Asr, .name = "a", .preset = "groq"});
    auto u = fx.svc->update(a.id, {.preset = "deepseek"});
    ASSERT_FALSE(u);
    EXPECT_EQ(u.error().code, ErrorCode::InvalidArgument);
    ASSERT_TRUE(fx.svc->update(a.id, {.preset = "openai"}));
    EXPECT_TRUE(fx.svc->get().profiles.size() == 1);
}

TEST(ModelProfiles, PlaceholderBaseUrlRejected) {
    Fixture fx;
    auto r = fx.svc->create({.kind = ModelKind::Llm, .name = "lan", .preset = "ollama"});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    // custom has no default address at all.
    r = fx.svc->create({.kind = ModelKind::Llm, .name = "c", .preset = "custom"});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(fx.svc->get().profiles.empty());

    ModelProfile ok = fx.make({.kind = ModelKind::Llm,
                               .name = "lan",
                               .preset = "ollama",
                               .base_url = "http://192.168.1.9:11434/v1"});
    EXPECT_EQ(ok.base_url, "http://192.168.1.9:11434/v1");
    auto u = fx.svc->update(ok.id, {.base_url = "http://<host>:11434/v1"});
    ASSERT_FALSE(u);
    EXPECT_EQ(u.error().code, ErrorCode::InvalidArgument);
}

TEST(ModelProfiles, LimitIsSixteen) {
    Fixture fx;
    for (int i = 0; i < 16; ++i) {
        const ModelKind kind = wisp::core::kModelKinds[i % 3];
        ASSERT_TRUE(
            fx.svc->create({.kind = kind, .name = "p" + std::to_string(i), .preset = "openai"}))
            << i;
    }
    ModelFailure why = ModelFailure::None;
    auto r = fx.svc->create({.kind = ModelKind::Tts, .name = "p16", .preset = "openai"}, &why);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(why, ModelFailure::LimitReached);
    EXPECT_STREQ(wisp::core::to_string(why), "limit_reached");
    EXPECT_EQ(fx.svc->get().profiles.size(), 16u);
}

TEST(ModelProfiles, RemoveActiveNeedsAnotherActivatedFirst) {
    Fixture fx;
    ModelProfile a = fx.make(llm("a"));
    ModelProfile b = fx.make(llm("b"));

    ModelFailure why = ModelFailure::None;
    auto r = fx.svc->remove(a.id, &why);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);
    EXPECT_EQ(why, ModelFailure::ActiveProfile);
    EXPECT_STREQ(wisp::core::to_string(why), "active_profile");

    // An inactive one goes at once.
    ASSERT_TRUE(fx.svc->remove(b.id));
    // The last of its kind goes too, leaving the kind without an active.
    ASSERT_TRUE(fx.svc->remove(a.id));
    auto s = fx.svc->get();
    EXPECT_TRUE(s.profiles.empty());
    EXPECT_EQ(s.active_for(ModelKind::Llm), "");

    auto missing = fx.svc->remove("00000000");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST(ModelProfiles, KeyNeverReadable) {
    wisp::log::init(64 * 1024);
    Fixture fx;
    auto created = fx.svc->create(llm("k", "https://api.example.com/v1", "sk-SECRET123"));
    ASSERT_TRUE(created);
    EXPECT_TRUE(created.value().has_key);
    auto updated = fx.svc->update(created.value().id, {.model = "gpt-x"});
    ASSERT_TRUE(updated);
    for (const Value& v :
         {wisp::core::to_value(created.value()), wisp::core::to_value(updated.value()),
          wisp::core::to_value(fx.svc->get())}) {
        const std::string json = wisp::to_json(v);
        EXPECT_EQ(json.find("SECRET"), std::string::npos) << json;
        EXPECT_EQ(json.find("api_key"), std::string::npos) << json;
        EXPECT_NE(json.find("has_key"), std::string::npos) << json;
    }
    EXPECT_EQ(wisp::log::ring_snapshot().find("SECRET"), std::string::npos);
    // The key is stored in the secrets file, and the clients get it.
    EXPECT_NE(Fixture::read(fx.models_path()).find("sk-SECRET123"), std::string::npos);
    auto active = fx.svc->active_for_request(ModelKind::Llm);
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->api_key, "sk-SECRET123");
    EXPECT_EQ(active->base_url, "https://api.example.com/v1");
    EXPECT_EQ(active->model, "gpt-x");
    EXPECT_FALSE(fx.svc->active_for_request(ModelKind::Asr).has_value());
}

TEST(ModelProfiles, OmittedKeyIsKeptEmptyKeyClears) {
    Fixture fx;
    ModelProfile p = fx.make(llm("p", "https://a.example", "sk-1"));
    ASSERT_TRUE(fx.svc->update(p.id, {.model = "m2"}));
    EXPECT_EQ(fx.key_of(p.id), "sk-1");
    auto r = fx.svc->update(p.id, {.api_key = ""});
    ASSERT_TRUE(r);
    EXPECT_FALSE(r.value().has_key);
    EXPECT_EQ(fx.key_of(p.id), "");
    EXPECT_EQ(Fixture::read(fx.models_path()).find("sk-1"), std::string::npos);
}

// The key belongs to the origin (scheme://host:port) it was saved for:
// moving base_url to another origin without a new key forgets the key, so
// it is never sent to a host the owner did not give it to.
TEST(ModelProfiles, OriginChangeWithoutKeyClearsKey) {
    {
        Fixture fx;
        ModelProfile p = fx.make(llm("p", "https://a.example/v1", "sk-old"));
        auto r = fx.svc->update(p.id, {.base_url = "https://b.example/v1"});
        ASSERT_TRUE(r);
        EXPECT_EQ(r.value().base_url, "https://b.example/v1");
        EXPECT_FALSE(r.value().has_key);
        EXPECT_EQ(Fixture::read(fx.models_path()).find("sk-old"), std::string::npos);
        fx.restart();
        EXPECT_FALSE(find(fx.svc->get(), p.id)->has_key);
    }
    // Every origin component counts.
    for (const char* next : {"http://a.example/v1", "https://a.example:8443/v1",
                             "https://a.example.evil/v1", "https://evil@b.example/v1"}) {
        Fixture fx;
        ModelProfile p = fx.make(llm("p", "https://a.example/v1", "sk-1"));
        auto r = fx.svc->update(p.id, {.base_url = std::string(next)});
        ASSERT_TRUE(r) << next;
        EXPECT_FALSE(r.value().has_key) << next;
    }
    // Path, host case, an explicit default port and user info do not
    // change where the request goes.
    {
        Fixture fx;
        ModelProfile p = fx.make(llm("p", "https://a.example/v1", "sk-1"));
        for (const char* next : {"https://a.example/v2", "https://A.Example/v1",
                                 "https://a.example:443/v1", "https://user@a.example/v1"}) {
            ASSERT_TRUE(fx.svc->update(p.id, {.base_url = std::string(next)}));
            EXPECT_EQ(fx.key_of(p.id), "sk-1") << next;
        }
        ASSERT_TRUE(fx.svc->update(p.id, {.base_url = "http://10.0.0.2/v1", .api_key = "sk-2"}));
        ASSERT_TRUE(fx.svc->update(p.id, {.base_url = "http://10.0.0.2:80"}));
        EXPECT_EQ(fx.key_of(p.id), "sk-2");
        ASSERT_TRUE(
            fx.svc->update(p.id, {.base_url = "http://[fd00::2]:8000/v1", .api_key = "sk-3"}));
        ASSERT_TRUE(fx.svc->update(p.id, {.base_url = "http://[FD00::2]:8000/v2"}));
        EXPECT_EQ(fx.key_of(p.id), "sk-3");
        ASSERT_TRUE(fx.svc->update(p.id, {.base_url = "http://[fd00::3]:8000/v2"}));
        EXPECT_EQ(fx.key_of(p.id), "");
        // A new origin with a new key keeps the new key.
        ASSERT_TRUE(fx.svc->update(p.id, {.base_url = "https://c.example/v1", .api_key = "sk-c"}));
        EXPECT_EQ(fx.key_of(p.id), "sk-c");
    }
}

TEST(ModelProfiles, KeyFromSameOriginCopies) {
    Fixture fx;
    ModelProfile src = fx.make(llm("src", "https://api.openai.com/v1", "sk-SHARED"));
    ModelProfile keyless = fx.make(llm("keyless", "https://api.openai.com/v1"));

    // Same origin, source has a key: copied inside the device.
    auto asr = fx.svc->create(
        {.kind = ModelKind::Asr, .name = "whisper", .preset = "openai", .api_key_from = src.id});
    ASSERT_TRUE(asr) << asr.error().message;
    EXPECT_TRUE(asr.value().has_key);
    EXPECT_EQ(fx.svc->active_for_request(ModelKind::Asr)->api_key, "sk-SHARED");

    // Another origin, a source without a key and an unknown source are all
    // key_origin_mismatch.
    for (const std::string& from : {src.id, keyless.id, std::string("ffffffff")}) {
        ModelFailure why = ModelFailure::None;
        const std::string url =
            from == src.id ? "https://api.groq.com/openai/v1" : "https://api.openai.com/v1";
        auto r = fx.svc->create({.kind = ModelKind::Asr,
                                 .name = "x",
                                 .preset = "groq",
                                 .base_url = url,
                                 .api_key_from = from},
                                &why);
        ASSERT_FALSE(r) << from;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(why, ModelFailure::KeyOriginMismatch) << from;
        EXPECT_EQ(r.error().message.find("SHARED"), std::string::npos);
    }
    EXPECT_STREQ(wisp::core::to_string(ModelFailure::KeyOriginMismatch), "key_origin_mismatch");

    // Both at once is a plain invalid_argument.
    ModelFailure why = ModelFailure::None;
    auto both = fx.svc->update(keyless.id, {.api_key = "sk-x", .api_key_from = src.id}, &why);
    ASSERT_FALSE(both);
    EXPECT_EQ(both.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(why, ModelFailure::None);

    // update() copies too, checked against the new base_url when both
    // change.
    auto u = fx.svc->update(keyless.id, {.api_key_from = src.id});
    ASSERT_TRUE(u);
    EXPECT_TRUE(u.value().has_key);
    EXPECT_EQ(fx.key_of(keyless.id), "sk-SHARED");
    ModelProfile other = fx.make(llm("other", "https://b.example/v1"));
    auto moved = fx.svc->update(
        other.id, {.base_url = "https://API.openai.com:443/v2", .api_key_from = src.id});
    ASSERT_TRUE(moved) << moved.error().message;
    EXPECT_EQ(fx.key_of(other.id), "sk-SHARED");
}

TEST(ModelProfiles, OptionsLimits) {
    Fixture fx;
    ModelProfile t = fx.make({.kind = ModelKind::Tts,
                              .name = "tts",
                              .preset = "openai",
                              .options = {{{"voice", "alloy"}, {"x_custom", "kept"}}}});
    EXPECT_EQ(t.options, (wisp::core::ModelOptions{{"voice", "alloy"}, {"x_custom", "kept"}}));

    wisp::core::ModelOptions nine;
    for (int i = 0; i < 9; ++i) nine["k" + std::to_string(i)] = "v";
    wisp::core::ModelOptions eight(nine);
    eight.erase("k8");
    ASSERT_TRUE(fx.svc->update(t.id, {.options = eight}));
    for (const wisp::core::ModelOptions& bad :
         {nine, wisp::core::ModelOptions{{std::string(16, 'k'), "v"}},
          wisp::core::ModelOptions{{"voice", std::string(65, 'v')}},
          wisp::core::ModelOptions{{"", "v"}}, wisp::core::ModelOptions{{"voice", "a\nb"}}}) {
        auto r = fx.svc->update(t.id, {.options = bad});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    ASSERT_TRUE(fx.svc->update(
        t.id, {.options = wisp::core::ModelOptions{{std::string(15, 'k'), std::string(64, 'v')}}}));
    fx.restart();
    EXPECT_EQ(find(fx.svc->get(), t.id)->options,
              (wisp::core::ModelOptions{{std::string(15, 'k'), std::string(64, 'v')}}));
}

TEST(ModelProfiles, ValidatesNameUrlModelAndKey) {
    Fixture fx;
    ModelProfile p = fx.make(llm("p", "http://10.0.0.2:8000/v1", "sk-keep"));
    for (const char* bad : {"ftp://x", "api.example.com", "", "https://", "http:///",
                            "javascript:1", "https://ex ample.com", "https://a.example/\r\nX: y",
                            // No host once user info and port are gone.
                            "https://@/", "https://:443", "https://user@:443/v1", "http://@",
                            "https://[]/v1", "https://[]:443/v1"}) {
        auto r = fx.svc->update(p.id, {.base_url = std::string(bad), .api_key = "sk-new"});
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << bad;
        EXPECT_EQ(r.error().message.find("sk-new"), std::string::npos);
    }
    for (const std::string& name :
         {std::string(), std::string(33, 'n'), std::string("a\x01"), std::string("\xff\xfe")}) {
        auto r = fx.svc->update(p.id, {.name = name});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    // 32 bytes of UTF-8, duplicates allowed.
    EXPECT_TRUE(fx.svc->update(p.id, {.name = "对话模型对话模型对话"}));  // 30 bytes
    EXPECT_TRUE(fx.svc->create(llm("对话模型对话模型对话")));
    EXPECT_TRUE(fx.svc->update(p.id, {.model = std::string(128, 'm')}));
    auto long_model = fx.svc->update(p.id, {.model = std::string(129, 'm')});
    ASSERT_FALSE(long_model);
    EXPECT_EQ(long_model.error().code, ErrorCode::InvalidArgument);
    for (const char* bad : {"sk-a\r\nX-Evil: 1", "sk a", "sk-\x01"}) {
        auto r = fx.svc->update(p.id, {.api_key = std::string(bad)});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(r.error().message.find("sk"), std::string::npos);
    }
    auto missing = fx.svc->update("12345678", {.name = "x"});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    // Every rejection left the profile alone.
    const auto state = fx.svc->get();
    const ModelProfile* now = find(state, p.id);
    EXPECT_EQ(now->base_url, "http://10.0.0.2:8000/v1");
    EXPECT_EQ(now->model, std::string(128, 'm'));
    EXPECT_EQ(fx.key_of(p.id), "sk-keep");
}

TEST(ModelProfiles, MigratesLegacyLlmJson) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(
        fx.legacy_path(),
        R"({"base_url":"https://api.example.com/v1","model":"gpt-x","api_key":"sk-LEGACY"})");
    fx.start();
    auto s = fx.svc->get();
    ASSERT_EQ(s.profiles.size(), 1u);
    const ModelProfile& p = s.profiles[0];
    EXPECT_EQ(p.kind, ModelKind::Llm);
    EXPECT_EQ(p.preset, "custom");
    EXPECT_EQ(p.name, "Default");
    EXPECT_EQ(p.base_url, "https://api.example.com/v1");
    EXPECT_EQ(p.model, "gpt-x");
    EXPECT_TRUE(p.has_key);
    EXPECT_EQ(s.active_for(ModelKind::Llm), p.id);
    EXPECT_EQ(fx.svc->active_for_request(ModelKind::Llm)->api_key, "sk-LEGACY");
    EXPECT_FALSE(std::filesystem::exists(fx.legacy_path()));
    EXPECT_NE(Fixture::read(fx.models_path()).find("\"version\":2"), std::string::npos);

    fx.restart();
    ASSERT_EQ(fx.svc->get().profiles.size(), 1u);
    EXPECT_EQ(fx.svc->get().profiles[0].id, p.id);
}

// A legacy key stored before any base_url was saved for no origin at all:
// the first base_url without a key forgets it.
TEST(ModelProfiles, MigratedUnboundKeyIsClearedByTheFirstBaseUrl) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.legacy_path(), R"({"base_url":"","model":"","api_key":"sk-early"})");
    fx.start();
    auto s = fx.svc->get();
    ASSERT_EQ(s.profiles.size(), 1u);
    EXPECT_TRUE(s.profiles[0].has_key);
    auto r = fx.svc->update(s.profiles[0].id, {.base_url = "https://a.example/v1"});
    ASSERT_TRUE(r);
    EXPECT_FALSE(r.value().has_key);
}

// A legacy URL that create() would refuse (here the Ollama placeholder)
// is not carried over: the profile keeps its model and key with no
// address, so the key stays unbound and the next load keeps the profile.
TEST(ModelProfiles, MigratedPlaceholderUrlBecomesUnbound) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.legacy_path(),
                   R"({"base_url":"http://<主机>:11434/v1","model":"qwen","api_key":"sk-old"})");
    fx.start();
    auto s = fx.svc->get();
    ASSERT_EQ(s.profiles.size(), 1u);
    EXPECT_EQ(s.profiles[0].base_url, "");
    EXPECT_EQ(s.profiles[0].model, "qwen");
    EXPECT_TRUE(s.profiles[0].has_key);

    fx.restart();
    s = fx.svc->get();
    ASSERT_EQ(s.profiles.size(), 1u);
    EXPECT_TRUE(s.profiles[0].has_key);
    auto r = fx.svc->update(s.profiles[0].id, {.base_url = "http://192.168.1.9:11434/v1"});
    ASSERT_TRUE(r);
    EXPECT_FALSE(r.value().has_key);
}

// An unreadable models.json next to a legacy file: the legacy file may hold
// the only good copy of the key, so the migration runs again and replaces
// the unreadable file.
TEST(ModelProfiles, CorruptModelsFileIsRebuiltFromTheLegacyFile) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.models_path(), "{not json");
    Fixture::write(fx.legacy_path(), R"({"base_url":"https://a.example/v1","api_key":"sk-only"})");
    fx.start();
    auto s = fx.svc->get();
    ASSERT_EQ(s.profiles.size(), 1u);
    EXPECT_TRUE(s.profiles[0].has_key);
    EXPECT_EQ(s.profiles[0].base_url, "https://a.example/v1");
    EXPECT_FALSE(std::filesystem::exists(fx.legacy_path()));
    EXPECT_EQ(fx.key_of(s.profiles[0].id), "sk-only");

    fx.restart();
    ASSERT_EQ(fx.svc->get().profiles.size(), 1u);
    EXPECT_EQ(fx.svc->get().profiles[0].id, s.profiles[0].id);
}

TEST(ModelProfiles, EmptyLegacyFileMigratesToNothing) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.legacy_path(), R"({"base_url":"","model":"","api_key":""})");
    fx.start();
    EXPECT_TRUE(fx.svc->get().profiles.empty());
    EXPECT_FALSE(std::filesystem::exists(fx.legacy_path()));
}

TEST(ModelProfiles, PersistsAcrossReload) {
    Fixture fx;
    ModelProfile l = fx.make(llm("chat", "https://api.example.com/v1", "sk-persist"));
    ModelProfile l2 = fx.make(llm("chat 2", "https://b.example/v1"));
    ModelProfile t = fx.make({.kind = ModelKind::Tts,
                              .name = "voice",
                              .preset = "openai",
                              .options = {{{"voice", "nova"}}}});
    ASSERT_TRUE(fx.svc->update(l.id, {.model = "gpt-x"}));
    ASSERT_TRUE(fx.svc->activate(l2.id));
    const auto before = fx.svc->get();

    fx.restart();
    const auto after = fx.svc->get();
    EXPECT_EQ(wisp::to_json(wisp::core::to_value(after)),
              wisp::to_json(wisp::core::to_value(before)));
    EXPECT_EQ(after.active_for(ModelKind::Llm), l2.id);
    EXPECT_EQ(after.active_for(ModelKind::Tts), t.id);
    EXPECT_EQ(fx.key_of(l.id), "sk-persist");
}

TEST(ModelProfiles, CorruptFileStartsEmpty) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.models_path(), "{not json");
    fx.start();
    EXPECT_TRUE(fx.svc->get().profiles.empty());
    // With nothing to rebuild from, the file is left for a person to look at.
    EXPECT_EQ(Fixture::read(fx.models_path()), "{not json");
}

// Hand-edited entries that create() would refuse are dropped, and an
// active id that names nothing falls back to the kind's first profile.
TEST(ModelProfiles, LoadDropsInvalidEntries) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.models_path(), R"({"version":2,"active":{"llm":"zzzzzzzz","asr":"","tts":""},
      "profiles":[
        {"id":"0000000a","kind":"llm","name":"ok","preset":"openai","base_url":"https://api.openai.com/v1","model":"","options":{},"api_key":"sk-1"},
        {"id":"0000000b","kind":"asr","name":"bad preset","preset":"deepseek","base_url":"https://api.deepseek.com/v1","model":"","options":{},"api_key":""},
        {"id":"0000000c","kind":"llm","name":"bad key","preset":"openai","base_url":"https://api.openai.com/v1","model":"","options":{},"api_key":"a b"},
        {"id":"0000000a","kind":"llm","name":"dup","preset":"openai","base_url":"https://api.openai.com/v1","model":"","options":{},"api_key":""},
        {"id":"0000000d","kind":"xyz","name":"bad kind","preset":"openai","base_url":"https://api.openai.com/v1","model":"","options":{},"api_key":""}
      ]})");
    fx.start();
    auto s = fx.svc->get();
    ASSERT_EQ(s.profiles.size(), 1u);
    EXPECT_EQ(s.profiles[0].id, "0000000a");
    EXPECT_EQ(s.active_for(ModelKind::Llm), "0000000a");
}

// A random source that keeps colliding (a broken platform RNG returning
// zeros) must not spin forever under the service lock.
TEST(ModelProfiles, CollidingRandomStillGivesUniqueIds) {
    Fixture fx;
    fx.svc.reset();
    ModelProfileService svc(*fx.storage, fx.bus,
                            [](uint8_t* out, size_t n) { std::fill(out, out + n, uint8_t{0}); });
    ASSERT_TRUE(svc.init());
    std::vector<std::string> ids;
    for (int i = 0; i < 4; ++i) {
        auto r = svc.create({.kind = ModelKind::Tts, .name = "t", .preset = "openai"});
        ASSERT_TRUE(r) << i;
        ids.push_back(r.value().id);
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::unique(ids.begin(), ids.end()), ids.end());
}

TEST(ModelProfiles, ConcurrentCreatesStopAtTheLimit) {
    Fixture fx;
    constexpr int kPerThread = 12;
    std::atomic<int> created{0};
    std::atomic<int> refused{0};
    auto worker = [&](ModelKind kind) {
        for (int i = 0; i < kPerThread; ++i) {
            ModelFailure why = ModelFailure::None;
            auto r = fx.svc->create({.kind = kind, .name = "p", .preset = "openai"}, &why);
            if (r) {
                ++created;
            } else if (r.error().code == ErrorCode::QuotaExceeded &&
                       why == ModelFailure::LimitReached) {
                ++refused;
            }
        }
    };
    std::thread a(worker, ModelKind::Llm);
    std::thread b(worker, ModelKind::Tts);
    a.join();
    b.join();
    EXPECT_EQ(created.load(), 16);
    EXPECT_EQ(refused.load(), 2 * kPerThread - 16);
    const auto state = fx.svc->get();
    EXPECT_EQ(state.profiles.size(), 16u);
    fx.restart();
    EXPECT_EQ(fx.svc->get().profiles.size(), 16u);
}

TEST(ModelProfiles, PublishesEventWithoutSecrets) {
    Fixture fx;
    std::mutex m;
    std::vector<Value> payloads;
    fx.bus.subscribe("model.profiles", [&](const wisp::core::Event& e) {
        std::lock_guard<std::mutex> lock(m);
        payloads.push_back(Value(e.payload));
    });
    ModelProfile p = fx.make(llm("p", "https://secret-host.example/v1", "sk-EVENT"));
    ASSERT_TRUE(fx.svc->update(p.id, {.model = "m"}));
    ASSERT_TRUE(fx.svc->activate(p.id));
    ASSERT_TRUE(fx.svc->remove(p.id));
    // A refused change publishes nothing.
    EXPECT_FALSE(fx.svc->remove(p.id));
    ASSERT_TRUE(fx.bus.wait_idle(2000));

    std::lock_guard<std::mutex> lock(m);
    ASSERT_EQ(payloads.size(), 4u);
    EXPECT_EQ(payloads[0],
              Value(Map{{"active",
                         Value(Map{{"llm", Value(p.id)}, {"asr", Value("")}, {"tts", Value("")}})},
                        {"count", Value(int64_t{1})}}));
    EXPECT_EQ(payloads[3],
              Value(Map{{"active",
                         Value(Map{{"llm", Value("")}, {"asr", Value("")}, {"tts", Value("")}})},
                        {"count", Value(int64_t{0})}}));
    for (const Value& v : payloads) {
        const std::string json = wisp::to_json(v);
        EXPECT_EQ(json.find("EVENT"), std::string::npos) << json;
        EXPECT_EQ(json.find("secret-host"), std::string::npos) << json;
    }
}

TEST(ModelProfiles, TestRunsTheProberOnTheProfile) {
    Fixture fx;
    ModelProfile a = fx.make(llm("a", "https://a.example/v1", "sk-A"));
    ModelProfile b = fx.make(llm("b", "https://b.example/v1", "sk-B"));

    auto none = fx.svc->test(a.id);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::Unsupported);

    ResolvedProfile seen;
    fx.svc->set_prober([&](const ResolvedProfile& t) {
        seen = t;
        ModelProbe p;
        p.ok = true;
        p.models = {"m1"};
        return p;
    });
    // Not only the active one can be tested.
    auto r = fx.svc->test(b.id);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r.value().ok);
    EXPECT_EQ(seen.id, b.id);
    EXPECT_EQ(seen.base_url, "https://b.example/v1");
    EXPECT_EQ(seen.api_key, "sk-B");
    auto missing = fx.svc->test("99999999");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST(ModelProfiles, ClearAllForFactoryReset) {
    Fixture fx;
    fx.make(llm("p", "https://a.example", "sk-gone"));
    fx.make({.kind = ModelKind::Asr, .name = "a", .preset = "openai"});
    Fixture::write(fx.legacy_path(),
                   R"({"base_url":"https://x","model":"","api_key":"sk-legacy"})");
    fx.svc->clear();
    EXPECT_TRUE(fx.svc->get().profiles.empty());
    EXPECT_FALSE(fx.svc->active_for_request(ModelKind::Llm).has_value());
    EXPECT_FALSE(std::filesystem::exists(fx.models_path()));
    EXPECT_FALSE(std::filesystem::exists(fx.legacy_path()));
    fx.restart();
    EXPECT_TRUE(fx.svc->get().profiles.empty());
}

}  // namespace

TEST(ModelProfiles, VisionAndContextWindowValidated) {
    Fixture fx;
    ModelProfile p = fx.make(llm("p"));
    for (const wisp::core::ModelOptions& bad :
         {wisp::core::ModelOptions{{"vision", "yes"}}, wisp::core::ModelOptions{{"vision", "TRUE"}},
          wisp::core::ModelOptions{{"context_window", "1000"}},
          wisp::core::ModelOptions{{"context_window", "x"}},
          wisp::core::ModelOptions{{"context_window", ""}},
          wisp::core::ModelOptions{{"context_window", "-4096"}},
          wisp::core::ModelOptions{{"context_window", "1048577"}},
          wisp::core::ModelOptions{{"context_window", "99999999999999999999"}}}) {
        auto r = fx.svc->update(p.id, {.options = bad});
        ASSERT_FALSE(r) << bad.begin()->second;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
        auto c = fx.svc->create({.kind = ModelKind::Llm,
                                 .name = "c",
                                 .preset = "custom",
                                 .base_url = "https://api.example.com/v1",
                                 .options = bad});
        EXPECT_FALSE(c);
    }
    auto ok = fx.svc->update(p.id, {.options = wisp::core::ModelOptions{
                                        {"vision", "true"}, {"context_window", "65536"}}});
    ASSERT_TRUE(ok);
    EXPECT_TRUE(wisp::core::profile_vision(ok.value().options));
    EXPECT_EQ(wisp::core::profile_context_window(ok.value().options), 65536u);
    for (const char* edge : {"4096", "1048576"}) {
        auto r = fx.svc->update(
            p.id, {.options = wisp::core::ModelOptions{{wisp::core::kOptContextWindow, edge}}});
        ASSERT_TRUE(r) << edge;
        EXPECT_EQ(wisp::core::profile_context_window(r.value().options),
                  static_cast<uint32_t>(std::stoul(edge)));
    }
    auto off = fx.svc->update(
        p.id, {.options = wisp::core::ModelOptions{{wisp::core::kOptVision, "false"}}});
    ASSERT_TRUE(off);
    EXPECT_FALSE(wisp::core::profile_vision(off.value().options));
}

TEST(ModelProfiles, ContextWindowDefaultsTo65536) {
    EXPECT_EQ(wisp::core::profile_context_window({}), 65536u);
    EXPECT_FALSE(wisp::core::profile_vision({}));
    Fixture fx;
    ModelProfile p = fx.make(llm("p"));
    EXPECT_EQ(wisp::core::profile_context_window(p.options), 65536u);
    EXPECT_FALSE(wisp::core::profile_vision(p.options));
}
