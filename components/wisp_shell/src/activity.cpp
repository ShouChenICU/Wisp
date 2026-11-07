#include "wisp/shell/activity.hpp"

#include <cstdio>
#include <initializer_list>

namespace wisp::shell {

namespace {

using ui::StrId;

struct ToolLabel {
    const char* tool;
    StrId label;
    bool target;  // whether the label names the thing the tool acts on
};

// Every tool the agent registers (wisp_agent's tools_*.cpp and docs.cpp).
constexpr ToolLabel kLabels[] = {
    {"scene.list", StrId::ActSceneList, false},
    {"scene.list_files", StrId::ActSceneListFiles, true},
    {"scene.read_file", StrId::ActSceneReadFile, true},
    {"scene.write_file", StrId::ActSceneWriteFile, true},
    {"scene.edit_file", StrId::ActSceneEditFile, true},
    {"scene.delete_file", StrId::ActSceneDeleteFile, true},
    {"scene.install", StrId::ActSceneInstall, true},
    {"scene.discard_draft", StrId::ActSceneDiscard, true},
    {"scene.rollback", StrId::ActSceneRollback, true},
    {"scene.open", StrId::ActSceneOpen, true},
    {"scene.close", StrId::ActSceneClose, true},
    {"scene.exit", StrId::ActSceneExit, false},
    {"scene.delete", StrId::ActSceneDelete, true},
    {"scene.eval", StrId::ActSceneEval, true},
    {"scene.logs", StrId::ActSceneLogs, true},
    {"scene.data_read", StrId::ActSceneDataRead, true},
    {"scene.data_write", StrId::ActSceneDataWrite, true},
    {"scene.files_list", StrId::ActSceneFilesList, true},
    {"scene.files_read", StrId::ActSceneFilesRead, true},
    {"scene.files_remove", StrId::ActSceneFilesRemove, true},
    // The memory's text is never shown; its id is.
    {"memory.add", StrId::ActMemoryAdd, false},
    {"memory.list", StrId::ActMemoryList, false},
    {"memory.update", StrId::ActMemoryUpdate, true},
    {"memory.remove", StrId::ActMemoryRemove, true},
    {"todo.add", StrId::ActTodoAdd, true},
    {"todo.list", StrId::ActTodoList, false},
    {"todo.update", StrId::ActTodoUpdate, true},
    {"todo.done", StrId::ActTodoDone, true},
    {"todo.remove", StrId::ActTodoRemove, true},
    // A settings value may be a key: never a target.
    {"settings.get", StrId::ActSettingsGet, false},
    {"settings.set", StrId::ActSettingsSet, false},
    {"device.status", StrId::ActDeviceStatus, false},
    {"notify.list", StrId::ActNotifyList, false},
    {"notify.dismiss", StrId::ActNotifyDismiss, false},
    {"docs.read", StrId::ActDocsRead, false},
    {"ui.tree", StrId::ActUiTree, false},
};

const ToolLabel* find_label(std::string_view tool) {
    for (const ToolLabel& l : kLabels)
        if (tool == l.tool) return &l;
    return nullptr;
}

// tr(id, lang) with each "{}" in turn replaced by the next of `values`.
os::LargeString fill(StrId id, ui::Lang lang, std::initializer_list<std::string_view> values) {
    const std::string_view text = ui::tr(id, lang);
    os::LargeString out;
    auto next = values.begin();
    for (size_t i = 0; i < text.size(); ++i) {
        if (next != values.end() && text[i] == '{' && i + 1 < text.size() && text[i + 1] == '}') {
            out.append(next->data(), next->size());
            ++next;
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

void append_utf8(os::LargeString& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Reads the JSON text from `s[i]`, just past an opening quote, to its
// closing quote or the end of `s`; `i` ends past the closing quote. With
// `out`, the decoded text goes there, control characters left out and
// unknown or cut escapes dropped.
void read_string(std::string_view s, size_t& i, os::LargeString* out) {
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return;
        if (c != '\\') {
            if (out != nullptr && static_cast<unsigned char>(c) >= 0x20) *out += c;
            continue;
        }
        if (i >= s.size()) return;
        const char e = s[i++];
        if (out == nullptr) {
            continue;
        }
        switch (e) {
            case '"':
            case '\\':
            case '/':
                *out += e;
                break;
            case 'u': {
                uint32_t cp = 0;
                bool ok = i + 4 <= s.size();
                for (size_t k = 0; ok && k < 4; ++k) {
                    const int d = hex_digit(s[i + k]);
                    ok = d >= 0;
                    cp = cp * 16 + static_cast<uint32_t>(d < 0 ? 0 : d);
                }
                if (!ok) {
                    i = s.size();
                    return;
                }
                i += 4;
                // Surrogates and control characters are left out.
                if (cp >= 0x20 && (cp < 0xD800 || cp > 0xDFFF)) append_utf8(*out, cp);
                break;
            }
            default:
                // \b \f \n \r \t: control characters.
                break;
        }
    }
}

void skip_space(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
}

int key_rank(const os::LargeString& key) {
    const std::string_view k(key.data(), key.size());
    if (k == "path") return 0;
    if (k == "name") return 1;
    if (k == "title") return 2;
    if (k == "id") return 3;
    return -1;
}

bool is_number_char(char c) {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

// At most `max` code points of `s`.
void cut_code_points(os::LargeString& s, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
        if (n++ == max) {
            s.resize(i);
            return;
        }
    }
}

}  // namespace

os::LargeString activity_target(std::string_view args) {
    os::LargeString best;
    int best_rank = 4;
    os::LargeString key;
    size_t i = 0;
    while (i < args.size()) {
        if (args[i] != '"') {
            ++i;
            continue;
        }
        ++i;
        key.clear();
        read_string(args, i, &key);
        skip_space(args, i);
        // A string not followed by ':' is a value (in a list): skipped whole.
        if (i >= args.size() || args[i] != ':') continue;
        ++i;
        skip_space(args, i);
        const int rank = key_rank(key);
        if (i >= args.size()) break;
        if (args[i] == '"') {
            ++i;
            if (rank >= 0 && rank < best_rank) {
                best.clear();
                read_string(args, i, &best);
                best_rank = rank;
            } else {
                read_string(args, i, nullptr);
            }
        } else if (is_number_char(args[i])) {
            const size_t start = i;
            while (i < args.size() && is_number_char(args[i])) ++i;
            if (rank >= 0 && rank < best_rank) {
                best.assign(args.data() + start, i - start);
                best_rank = rank;
            }
        }
        // Objects and lists are read on: their keys count too.
    }
    cut_code_points(best, kActivityTargetChars);
    return best;
}

os::LargeString activity_label(std::string_view tool, std::string_view args, ui::Lang lang) {
    const ToolLabel* l = find_label(tool);
    if (l == nullptr) return fill(StrId::ActivityCall, lang, {tool});
    os::LargeString out(ui::tr(l->label, lang));
    if (l->target) {
        const os::LargeString target = activity_target(args);
        if (!target.empty()) {
            out += ' ';
            out += target;
        }
    }
    return out;
}

os::LargeString activity_prep_label(std::string_view tool, uint32_t bytes, ui::Lang lang) {
    os::LargeString out = activity_label(tool, {}, lang);
    if (bytes == 0) return out;
    char size[24];
    if (bytes < 1024) {
        std::snprintf(size, sizeof size, "%u B", static_cast<unsigned>(bytes));
    } else if (bytes < 1024u * 1024u) {
        std::snprintf(size, sizeof size, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(size, sizeof size, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    out += " · ";
    out += size;
    return out;
}

os::LargeString activity_summary(uint32_t steps, uint64_t elapsed_ms, bool running, ui::Lang lang) {
    const uint64_t total_s = elapsed_ms / 1000;
    char n[12], a[24], b[12];
    std::snprintf(n, sizeof n, "%u", static_cast<unsigned>(steps));
    os::LargeString time;
    if (total_s < 60) {
        std::snprintf(a, sizeof a, "%u", static_cast<unsigned>(total_s));
        time = fill(StrId::ActivitySeconds, lang, {a});
    } else {
        std::snprintf(a, sizeof a, "%llu", static_cast<unsigned long long>(total_s / 60));
        std::snprintf(b, sizeof b, "%u", static_cast<unsigned>(total_s % 60));
        time = fill(StrId::ActivityMinSec, lang, {a, b});
    }
    return fill(running ? StrId::ActivityStep : StrId::ActivityDone, lang,
                {n, std::string_view(time.data(), time.size())});
}

}  // namespace wisp::shell
