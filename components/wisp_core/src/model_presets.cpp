// The compiled-in platform table (spec 5.3). Adding a platform is one row.
// Model names are deliberately not pre-filled: platforms rename them too
// often, so the user picks one from the connectivity test's list.

#include <array>

#include "wisp/core/model_profiles.hpp"

namespace wisp::core {

namespace {

constexpr uint8_t bit(ModelKind k) { return static_cast<uint8_t>(1u << static_cast<unsigned>(k)); }

constexpr uint8_t kLlm = bit(ModelKind::Llm);
constexpr uint8_t kAsr = bit(ModelKind::Asr);
constexpr uint8_t kTts = bit(ModelKind::Tts);

constexpr std::array<ModelPreset, 13> kPresets = {{
    {"custom", "OpenAI 兼容（自定义）", kLlm | kAsr | kTts, "", false},
    {"openai", "OpenAI", kLlm | kAsr | kTts, "https://api.openai.com/v1", true},
    {"anthropic", "Anthropic", kLlm, "https://api.anthropic.com/v1", true},
    {"gemini", "Google Gemini", kLlm, "https://generativelanguage.googleapis.com/v1beta/openai",
     true},
    {"deepseek", "DeepSeek", kLlm, "https://api.deepseek.com/v1", true},
    {"qwen", "通义千问", kLlm, "https://dashscope.aliyuncs.com/compatible-mode/v1", true},
    {"moonshot", "Kimi", kLlm, "https://api.moonshot.cn/v1", true},
    {"zhipu", "智谱 GLM", kLlm, "https://open.bigmodel.cn/api/paas/v4", true},
    {"ark", "火山方舟", kLlm, "https://ark.cn-beijing.volces.com/api/v3", true},
    {"siliconflow", "硅基流动", kLlm | kAsr | kTts, "https://api.siliconflow.cn/v1", true},
    {"groq", "Groq", kLlm | kAsr, "https://api.groq.com/openai/v1", true},
    {"openrouter", "OpenRouter", kLlm, "https://openrouter.ai/api/v1", true},
    // A placeholder: the user has to put in the LAN host.
    {"ollama", "Ollama（局域网）", kLlm, "http://<主机>:11434/v1", false},
}};

}  // namespace

std::span<const ModelPreset> model_presets() { return kPresets; }

const ModelPreset* find_model_preset(std::string_view id) {
    for (const ModelPreset& p : kPresets) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

}  // namespace wisp::core
