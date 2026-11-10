#include "wisp/net/model_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "support/fake_http.hpp"
#include "wisp/core/model_profiles.hpp"

using wisp::core::ModelInfo;
using wisp::core::ModelKind;
using wisp::core::ModelProbe;
using wisp::core::ResolvedProfile;
using wisp::net::Method;
using wisp::net::parse_model_list;
using wisp::net::probe_models;
using wisp::net::TransportError;
using wisp::test::FakeHttpClient;

namespace {

struct Fixture {
    // Each call advances the clock by 120 ms, so a probe that reads it
    // once before and once after the request measures 120 ms.
    wisp::core::ModelProbe probe(const std::string& base_url, const std::string& key) {
        ResolvedProfile target{.id = "0000abcd", .base_url = base_url, .api_key = key};
        return probe_models(target, client, [this] {
            uint64_t t = clock;
            clock += 120;
            return t;
        });
    }

    FakeHttpClient client;
    uint64_t clock = 5000;
};

std::vector<std::string> ids(const wisp::core::ModelProbe& p) {
    std::vector<std::string> out;
    for (const auto& m : p.models) out.emplace_back(m);
    return out;
}

std::string header(const wisp::net::HttpClientRequest& req, const std::string& name) {
    for (const auto& [k, v] : req.headers) {
        if (k == name) return v;
    }
    return {};
}

TEST(ModelProbe, ParsesModelsAndSendsBearer) {
    Fixture f;
    f.client.next = {.status = 200,
                     .body = R"({"object":"list","data":[{"id":"gpt-a","object":"model"},)"
                             R"({"id":"gpt-b"},{"object":"model"}]})"};

    auto p = f.probe("https://api.example.com/v1", "sk-test123");

    auto req = f.client.last();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, Method::Get);
    EXPECT_EQ(req->url, "https://api.example.com/v1/models");
    EXPECT_EQ(header(*req, "Authorization"), "Bearer sk-test123");
    EXPECT_EQ(req->timeout_ms, 10000u);
    // The key goes only into the Authorization header.
    EXPECT_EQ(req->url.find("sk-test123"), std::string::npos);
    EXPECT_TRUE(req->body.empty());

    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.error, "");
    EXPECT_EQ(p.http_status, 200);
    EXPECT_EQ(p.latency_ms, 120u);
    EXPECT_EQ(ids(p), (std::vector<std::string>{"gpt-a", "gpt-b"}));
    EXPECT_TRUE(p.details.empty());
}

TEST(ModelProbe, MapsTransportAndHttpErrors) {
    Fixture f;
    struct Case {
        TransportError transport;
        int status;
        const char* body;
        const char* error;
    };
    for (const Case& c :
         {Case{TransportError::Dns, 0, "", "dns"}, Case{TransportError::Connect, 0, "", "connect"},
          Case{TransportError::Tls, 0, "", "tls"}, Case{TransportError::Timeout, 0, "", "timeout"},
          Case{TransportError::Other, 0, "", "other"},
          Case{TransportError::TooLarge, 0, "", "other"},
          Case{TransportError::None, 401, R"({"error":"bad key"})", "http"},
          Case{TransportError::None, 503, "", "http"},
          // A 200 whose body is not a model list is not a
          // working endpoint.
          Case{TransportError::None, 200, "<html>captive portal</html>", "http"},
          Case{TransportError::None, 200, R"({"data":"nope"})", "http"}}) {
        f.client.next = {.transport = c.transport, .status = c.status, .body = c.body};
        auto p = f.probe("https://x/v1", "sk-abc");
        EXPECT_FALSE(p.ok) << c.error;
        EXPECT_EQ(p.error, c.error);
        EXPECT_EQ(p.http_status, c.status) << c.error;
        EXPECT_TRUE(p.models.empty());
    }
}

// Several platforms (Anthropic's OpenAI layer, some ASR/TTS services) have
// no /models; the page then asks for the model name by hand (spec 5.3).
TEST(ModelProbe, NotFoundIsNoModelList) {
    Fixture f;
    f.client.next = {.status = 404, .body = R"({"error":"not found"})"};
    auto p = f.probe("https://api.anthropic.com/v1", "sk-ant");
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.error, "no_model_list");
    EXPECT_EQ(p.http_status, 404);
}

TEST(ModelProbe, WithoutBaseUrlSendsNothing) {
    Fixture f;
    auto p = f.probe("", "sk-1");
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.error, "other");
    EXPECT_EQ(f.client.calls(), 0u);
}

TEST(ModelProbe, WithoutKeySendsNoAuthorization) {
    Fixture f;
    f.client.next = {.status = 200, .body = R"({"data":[{"id":"qwen"}]})"};
    auto p = f.probe("http://192.168.1.9:11434/v1", "");
    EXPECT_TRUE(p.ok);
    auto req = f.client.last();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(header(*req, "Authorization"), "");
}

TEST(ModelProbe, DropsModelIdsThatEchoTheKey) {
    Fixture f;
    f.client.next = {.status = 200,
                     .body = R"({"data":[{"id":"ok-model"},{"id":"echo-sk-SECRETVALUE"}]})"};
    auto p = f.probe("https://x/v1", "sk-SECRETVALUE");
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(ids(p), (std::vector<std::string>{"ok-model"}));
}

TEST(ModelProbe, ProberAdaptsTheService) {
    Fixture f;
    f.client.next = {.status = 200, .body = R"({"data":[{"id":"m"}]})"};
    auto prober = wisp::net::make_model_prober(f.client, [] { return uint64_t{0}; });
    auto p = prober(ResolvedProfile{.base_url = "https://y/v1"});
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(f.client.last()->url, "https://y/v1/models");
}

// GET /v1/models of a speech.cpp server (tests/net/fixtures), one
// synthesis and one recognition model.
std::string speech_cpp_fixture() {
    std::ifstream in(std::string(WISP_NET_FIXTURE_DIR) + "/speech_cpp_models.json");
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const ModelInfo* detail(const ModelProbe& p, std::string_view id) {
    for (const ModelInfo& d : p.details) {
        if (std::string_view(d.id) == id) return &d;
    }
    return nullptr;
}

std::vector<std::string> strings(const wisp::os::LargeVector<wisp::os::LargeString>& v) {
    std::vector<std::string> out;
    for (const auto& s : v) out.emplace_back(s);
    return out;
}

TEST(ModelProbe, ReadsTheBodyIntoLargeMemoryUnderACap) {
    Fixture f;
    f.client.next = {.status = 200, .body = R"({"data":[{"id":"m"}]})"};
    auto p = f.probe("https://x/v1", "");
    EXPECT_TRUE(p.ok);
    auto req = f.client.last();
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->max_response_bytes, wisp::net::kModelProbeMaxBytes);
    EXPECT_TRUE(static_cast<bool>(req->on_body));

    // Bigger than one 4 KB piece of the fake client.
    std::string big = R"({"data":[)";
    for (int i = 0; i < 200; ++i) big += R"({"id":"model-)" + std::to_string(i) + R"("},)";
    big += R"({"id":"last"}]})";
    f.client.next = {.status = 200, .body = big};
    p = f.probe("https://x/v1", "");
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.models.size(), 201u);
    EXPECT_EQ(std::string_view(p.models.back()), "last");

    f.client.next = {.transport = TransportError::TooLarge};
    p = f.probe("https://x/v1", "");
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.error, "other");
}

TEST(ModelProbe, SpeechCppDetails) {
    Fixture f;
    const std::string body = speech_cpp_fixture();
    ASSERT_FALSE(body.empty());
    f.client.next = {.status = 200, .body = body};
    auto p = f.probe("http://10.0.0.100:8080/v1", "");
    ASSERT_TRUE(p.ok) << p.error;
    EXPECT_EQ(ids(p),
              (std::vector<std::string>{"Qwen3-TTS-12Hz-0.6B-CustomVoice", "Qwen3-ASR-1.7B"}));
    ASSERT_EQ(p.details.size(), 2u);

    const ModelInfo* tts = detail(p, "Qwen3-TTS-12Hz-0.6B-CustomVoice");
    ASSERT_NE(tts, nullptr);
    EXPECT_EQ(tts->kind, ModelKind::Tts);
    EXPECT_EQ(tts->sample_rate, 24000u);
    EXPECT_EQ(strings(tts->languages), (std::vector<std::string>{"de", "en", "es", "fr", "it", "ja",
                                                                 "ko", "pt", "ru", "zh"}));
    ASSERT_EQ(tts->voices.size(), 9u);
    EXPECT_EQ(std::string_view(tts->voices[0].name), "aiden");
    const auto& serena = tts->voices[5];
    EXPECT_EQ(std::string_view(serena.name), "serena");
    EXPECT_EQ(std::string_view(serena.language), "zh");
    EXPECT_EQ(std::string_view(serena.gender), "female");
    EXPECT_EQ(std::string_view(serena.description), "Warm, gentle young female voice.");
    EXPECT_EQ(tts->context_window, 0u);
    EXPECT_FALSE(tts->vision.has_value());

    const ModelInfo* asr = detail(p, "Qwen3-ASR-1.7B");
    ASSERT_NE(asr, nullptr);
    EXPECT_EQ(asr->kind, ModelKind::Asr);
    EXPECT_EQ(asr->sample_rate, 16000u);
    EXPECT_EQ(asr->languages.size(), 30u);
    EXPECT_EQ(std::string_view(asr->languages[29]), "zh");
    EXPECT_TRUE(asr->voices.empty());
}

TEST(ModelProbe, PlainOpenAiListHasNoDetails) {
    ModelProbe p;
    ASSERT_TRUE(parse_model_list(
        R"({"object":"list","data":[{"id":"gpt-4o","object":"model","created":1715367049,)"
        R"("owned_by":"system"},{"id":"whisper-1","object":"model"}]})",
        "", p));
    EXPECT_EQ(ids(p), (std::vector<std::string>{"gpt-4o", "whisper-1"}));
    EXPECT_TRUE(p.details.empty());
}

TEST(ModelProbe, OpenRouterContextAndVision) {
    ModelProbe p;
    ASSERT_TRUE(parse_model_list(
        R"({"data":[)"
        R"({"id":"openai/gpt-4o","name":"GPT-4o","context_length":128000,)"
        R"("architecture":{"modality":"text+image->text","input_modalities":["text","image"],)"
        R"("output_modalities":["text"]},"pricing":{"prompt":"0.0000025"}},)"
        R"({"id":"meta/llama","context_length":32768,"architecture":{"input_modalities":["text"]}},)"
        R"({"id":"old/style","architecture":{"modality":"text+image->text"}},)"
        R"({"id":"groq/x","context_window":131072},)"
        R"({"id":"bad/numbers","context_length":-5,"speech":{"sample_rate":1.5e99}}]})",
        "", p));
    EXPECT_EQ(p.models.size(), 5u);
    ASSERT_EQ(p.details.size(), 4u);
    const ModelInfo* gpt = detail(p, "openai/gpt-4o");
    ASSERT_NE(gpt, nullptr);
    EXPECT_EQ(gpt->context_window, 128000u);
    EXPECT_EQ(gpt->vision, true);
    EXPECT_FALSE(gpt->kind.has_value());
    const ModelInfo* llama = detail(p, "meta/llama");
    ASSERT_NE(llama, nullptr);
    EXPECT_EQ(llama->context_window, 32768u);
    EXPECT_EQ(llama->vision, false);
    const ModelInfo* old = detail(p, "old/style");
    ASSERT_NE(old, nullptr);
    EXPECT_EQ(old->vision, true);
    EXPECT_EQ(old->context_window, 0u);
    const ModelInfo* groq = detail(p, "groq/x");
    ASSERT_NE(groq, nullptr);
    EXPECT_EQ(groq->context_window, 131072u);
    EXPECT_EQ(detail(p, "bad/numbers"), nullptr);
}

TEST(ModelProbe, MalformedListsAreRefused) {
    for (std::string_view body :
         {std::string_view(""), std::string_view("null"), std::string_view("[]"),
          std::string_view(R"({"data":{}})"), std::string_view(R"({"data":[{"id":"a"})"),
          std::string_view(R"({"data":[{"id":"a\q"}]})"), std::string_view(R"({"data":[1 2]})"),
          std::string_view(R"({"data":[{"id":"a","speech":{"voices":[{"name":"x"]}}]})"),
          std::string_view(R"({"data":[]} trailing)")}) {
        ModelProbe p;
        EXPECT_FALSE(parse_model_list(body, "", p)) << body;
    }
    // Deep nesting in a field nobody reads is refused without recursing.
    std::string deep = R"({"data":[{"id":"a","x":)";
    deep += std::string(100000, '[');
    deep += std::string(100000, ']');
    deep += "}]}";
    ModelProbe p;
    EXPECT_FALSE(parse_model_list(deep, "", p));
    // Wrong types are skipped, not fatal.
    ModelProbe q;
    ASSERT_TRUE(parse_model_list(
        R"({"data":[{"id":7},{"id":"ok","speech":"no","context_length":"big"},)"
        R"({"id":"s","speech":{"task":5,"languages":[1,"en",null],"voices":[3,{"name":4},)"
        R"({"gender":"male"},{"name":"v","language":["x"]}]}}]})",
        "", q));
    EXPECT_EQ(ids(q), (std::vector<std::string>{"ok", "s"}));
    ASSERT_EQ(q.details.size(), 1u);
    EXPECT_EQ(strings(q.details[0].languages), (std::vector<std::string>{"en"}));
    ASSERT_EQ(q.details[0].voices.size(), 1u);
    EXPECT_EQ(std::string_view(q.details[0].voices[0].name), "v");
    EXPECT_TRUE(q.details[0].voices[0].language.empty());
    EXPECT_FALSE(q.details[0].kind.has_value());
}

TEST(ModelProbe, ListsAreBounded) {
    std::string body = R"({"data":[)";
    for (size_t i = 0; i < wisp::net::kMaxListedModels + 20; ++i) {
        body += R"({"id":"m)" + std::to_string(i) + R"(","speech":{"task":"synthesis","voices":[)";
        for (size_t v = 0; v < wisp::net::kMaxModelVoices + 5; ++v)
            body += R"({"name":"v)" + std::to_string(v) + R"("},)";
        body += R"({"name":"end"}],"languages":[)";
        for (size_t l = 0; l < wisp::net::kMaxModelLanguages + 5; ++l) body += R"("en",)";
        body += R"("zh"]}},)";
    }
    body += R"({"id":")" + std::string(wisp::net::kMaxModelIdBytes + 1, 'x') + R"("}]})";
    ModelProbe p;
    ASSERT_TRUE(parse_model_list(body, "", p));
    EXPECT_EQ(p.models.size(), wisp::net::kMaxListedModels);
    EXPECT_EQ(p.details.size(), wisp::net::kMaxListedModels);
    size_t voices = 0;
    for (const ModelInfo& d : p.details) {
        EXPECT_LE(d.voices.size(), wisp::net::kMaxModelVoices);
        EXPECT_EQ(d.languages.size(), wisp::net::kMaxModelLanguages);
        voices += d.voices.size();
    }
    EXPECT_EQ(voices, wisp::net::kMaxListedVoices);
    EXPECT_EQ(p.details[0].voices.size(), wisp::net::kMaxModelVoices);

    // An id over the limit is dropped, one at it is kept.
    ModelProbe q;
    ASSERT_TRUE(parse_model_list(
        R"({"data":[{"id":")" + std::string(wisp::net::kMaxModelIdBytes + 1, 'x') +
            R"("},{"id":")" + std::string(wisp::net::kMaxModelIdBytes, 'y') + R"("}]})",
        "", q));
    ASSERT_EQ(q.models.size(), 1u);
    EXPECT_EQ(q.models[0].size(), wisp::net::kMaxModelIdBytes);
}

TEST(ModelProbe, StringsAreDecodedAndCut) {
    std::string longname(wisp::net::kMaxVoiceNameBytes + 1, 'n');
    std::string desc;
    for (int i = 0; i < 100; ++i) desc += "\xe4\xb8\xad";  // U+4E2D, 3 bytes
    ModelProbe p;
    ASSERT_TRUE(parse_model_list(R"({"data":[{"id":"tést","speech":{"task":"synthesis","voices":[)"
                                 R"({"name":"a\"b","description":"中文 😀\n"},)"
                                 R"({"name":")" +
                                     longname +
                                     R"("},)"
                                     R"({"name":"cut","description":")" +
                                     desc + R"("}]}}]})",
                                 "", p));
    ASSERT_EQ(p.details.size(), 1u);
    EXPECT_EQ(std::string_view(p.models[0]), "t\xc3\xa9st");
    const auto& v = p.details[0].voices;
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(std::string_view(v[0].name), "a\"b");
    EXPECT_EQ(std::string_view(v[0].description), "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80\n");
    EXPECT_EQ(std::string_view(v[1].name), "cut");
    const std::string_view cut(v[1].description);
    EXPECT_LE(cut.size(), wisp::net::kMaxVoiceDescriptionBytes);
    EXPECT_GT(cut.size(), wisp::net::kMaxVoiceDescriptionBytes - 6);
    EXPECT_TRUE(cut.ends_with("\xe2\x80\xa6"));
    // Whole characters only before the ellipsis.
    EXPECT_EQ((cut.size() - 3) % 3, 0u);
}

TEST(ModelProbe, DetailsThatEchoTheKeyAreDropped) {
    ModelProbe p;
    ASSERT_TRUE(parse_model_list(
        R"({"data":[{"id":"m","speech":{"task":"synthesis","languages":["sk-KEY1","en"],)"
        R"("voices":[{"name":"ok"},{"name":"v","description":"has sk-KEY1 in it"},)"
        R"({"name":"sk-KEY1"}]}}]})",
        "sk-KEY1", p));
    ASSERT_EQ(p.details.size(), 1u);
    EXPECT_EQ(strings(p.details[0].languages), (std::vector<std::string>{"en"}));
    ASSERT_EQ(p.details[0].voices.size(), 1u);
    EXPECT_EQ(std::string_view(p.details[0].voices[0].name), "ok");
}

}  // namespace
