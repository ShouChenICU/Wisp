// The history part of a model request, and compression
// (wisp/agent/context.hpp).
//
// The history as the model sees it is a list of units: each message that
// goes on the wire, with every live summary standing in once for the
// messages it covers, at the place of the first of them. A summary is live
// unless a later one covers all it covers (a summary folded into a newer
// one); an empty summary covers turns dropped without one. Notes and
// markers are for people only and send nothing. A call without a stored
// result (a turn cut short by a restart) is left out, and so is a result
// without its call: the API refuses either.

#include "wisp/agent/context.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_registry.hpp"

namespace wisp::agent {

namespace {

constexpr std::string_view kSummaryLead = "Summary of the earlier conversation:\n";

constexpr std::string_view kSummaryPrompt =
    "Summarize the conversation transcript in the next message for your own use later in the "
    "same conversation: the transcript will be dropped and only your summary kept. Keep what "
    "still matters: what the user asked for and decided, their preferences, the scenes created "
    "or changed (ids, files, versions), problems left open and anything promised. Leave out "
    "greetings and tool output that no longer matters. Write plain sentences in the language of "
    "the conversation, at most 300 words, without <say> tags or Markdown.";

std::string_view sv(const os::LargeString& s) { return {s.data(), s.size()}; }

// Bytes in the longest prefix of `s` of at most `max_bytes` bytes that
// does not split a character.
size_t prefix_bytes(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s.size();
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

std::string cut_note(size_t total) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "\n[truncated, %zu bytes total]", total);
    return buf;
}

struct Unit {
    const Message* m = nullptr;
    bool summary = false;
    uint64_t first = 0, last = 0;  // the seqs it stands for
};

bool in_sorted(const os::LargeVector<std::string_view>& v, std::string_view id) {
    return std::binary_search(v.begin(), v.end(), id);
}

class Layout {
public:
    explicit Layout(std::span<const Message> h) {
        find_live(h);
        os::LargeVector<bool> emitted(live_.size(), false);
        for (const Message& m : h) {
            if (m.role == Role::Summary) continue;
            const size_t c = covering(m.seq);
            if (c != kNone) {
                if (!emitted[c]) {
                    emitted[c] = true;
                    const Message* s = live_[c];
                    units_.push_back({s, true, s->covers_first, s->covers_last});
                }
                continue;
            }
            units_.push_back({&m, false, m.seq, m.seq});
        }

        for (const Unit& u : units_)
            if (!u.summary && u.m->role == Role::Tool) answered_.push_back(sv(u.m->call_id));
        std::sort(answered_.begin(), answered_.end());
        for (const Unit& u : units_) {
            if (u.summary || u.m->role != Role::Assistant) continue;
            for (const ToolCallRec& c : u.m->calls)
                if (in_sorted(answered_, sv(c.id))) asked_.push_back(sv(c.id));
        }
        std::sort(asked_.begin(), asked_.end());
    }

    const os::LargeVector<Unit>& units() const { return units_; }

    bool kept(const ToolCallRec& c) const { return in_sorted(answered_, sv(c.id)); }

    // Bytes the unit puts on the wire (text, call ids, names, arguments).
    size_t bytes(const Unit& u) const {
        const Message& m = *u.m;
        if (u.summary) return m.text.empty() ? 0 : kSummaryLead.size() + m.text.size();
        switch (m.role) {
            case Role::User:
                return m.text.size();
            case Role::Assistant: {
                size_t n = m.text.size();
                for (const ToolCallRec& c : m.calls)
                    if (kept(c)) n += c.id.size() + c.name.size() + c.args.size();
                return n;
            }
            case Role::Tool:
                if (!in_sorted(asked_, sv(m.call_id))) return 0;
                return m.call_id.size() + tool_text_bytes(m);
            case Role::Note:
            case Role::Summary:
            case Role::Marker:
                break;
        }
        return 0;
    }

    // Appends the unit's message (if it sends one) to `r`. A text built in
    // the arena is recorded as `at` (offset, length) for message `which`,
    // and pointed at by settle() once the arena stops growing.
    void emit(const Unit& u, BuiltRequest& r, os::LargeVector<std::pair<size_t, size_t>>& at,
              os::LargeVector<size_t>& which) const {
        const Message& m = *u.m;
        auto arena_text = [&](net::LlmRole role, std::string_view a, std::string_view b) {
            net::LlmMessage& out = r.messages.emplace_back();
            out.role = role;
            which.push_back(r.messages.size() - 1);
            at.emplace_back(r.arena.size(), a.size() + b.size());
            r.arena.append(a.data(), a.size());
            r.arena.append(b.data(), b.size());
            return r.messages.size() - 1;
        };
        if (u.summary) {
            if (!m.text.empty()) arena_text(net::LlmRole::System, kSummaryLead, sv(m.text));
            return;
        }
        switch (m.role) {
            case Role::User: {
                net::LlmMessage& out = r.messages.emplace_back();
                out.role = net::LlmRole::User;
                out.text = sv(m.text);
                break;
            }
            case Role::Assistant: {
                os::LargeVector<net::LlmToolCall> calls;
                for (const ToolCallRec& c : m.calls) {
                    if (!kept(c)) continue;
                    calls.push_back({.id = std::string(sv(c.id)),
                                     .name = wire_name(sv(c.name)),
                                     .arguments = c.args});
                }
                if (m.text.empty() && calls.empty()) break;
                r.calls.push_back(std::move(calls));
                net::LlmMessage& out = r.messages.emplace_back();
                out.role = net::LlmRole::Assistant;
                out.text = sv(m.text);
                // The inner vector's buffer stays put when `calls` grows.
                out.tool_calls = {r.calls.back().data(), r.calls.back().size()};
                break;
            }
            case Role::Tool: {
                if (!in_sorted(asked_, sv(m.call_id))) break;
                size_t idx;
                if (m.text.size() <= kToolResultKeepBytes) {
                    net::LlmMessage& out = r.messages.emplace_back();
                    out.role = net::LlmRole::Tool;
                    out.text = sv(m.text);
                    idx = r.messages.size() - 1;
                } else {
                    const std::string_view t = sv(m.text);
                    idx = arena_text(net::LlmRole::Tool,
                                     t.substr(0, prefix_bytes(t, kToolResultKeepBytes)),
                                     cut_note(t.size()));
                }
                r.messages[idx].tool_call_id = sv(m.call_id);
                break;
            }
            case Role::Note:
            case Role::Summary:
            case Role::Marker:
                break;
        }
    }

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);

    static size_t tool_text_bytes(const Message& m) {
        if (m.text.size() <= kToolResultKeepBytes) return m.text.size();
        return prefix_bytes(sv(m.text), kToolResultKeepBytes) + cut_note(m.text.size()).size();
    }

    void find_live(std::span<const Message> h) {
        os::LargeVector<const Message*> all;
        for (const Message& m : h)
            if (m.role == Role::Summary && m.covers_first <= m.covers_last) all.push_back(&m);
        for (const Message* s : all) {
            bool superseded = false;
            for (const Message* t : all) {
                if (t == s) continue;
                const bool inside =
                    t->covers_first <= s->covers_first && s->covers_last <= t->covers_last;
                const bool same =
                    t->covers_first == s->covers_first && t->covers_last == s->covers_last;
                if (inside && (!same || t->seq > s->seq)) {
                    superseded = true;
                    break;
                }
            }
            if (!superseded) live_.push_back(s);
        }
        std::sort(live_.begin(), live_.end(), [](const Message* a, const Message* b) {
            return a->covers_first < b->covers_first;
        });
    }

    // The live summary covering `seq`, or kNone.
    size_t covering(uint64_t seq) const {
        auto it =
            std::upper_bound(live_.begin(), live_.end(), seq,
                             [](uint64_t s, const Message* m) { return s < m->covers_first; });
        if (it == live_.begin()) return kNone;
        --it;
        return seq <= (*it)->covers_last ? static_cast<size_t>(it - live_.begin()) : kNone;
    }

    os::LargeVector<const Message*> live_;  // by covers_first
    os::LargeVector<Unit> units_;
    os::LargeVector<std::string_view> answered_, asked_;
};

// Points the arena messages at their text, now that the arena is final.
void settle(BuiltRequest& r, const os::LargeVector<std::pair<size_t, size_t>>& at,
            const os::LargeVector<size_t>& which) {
    for (size_t i = 0; i < which.size(); ++i)
        r.messages[which[i]].text = std::string_view(r.arena).substr(at[i].first, at[i].second);
}

BuiltRequest build(std::span<const Message> history, const DeviceSnapshot* d) {
    BuiltRequest r;
    const Layout layout(history);
    std::string prompt, info;
    if (d != nullptr) {
        prompt = system_prompt(*d);
        info = dynamic_info(*d);
    }
    // Always on the heap, so a move keeps every view valid.
    r.arena.reserve(std::max<size_t>(64, prompt.size() + info.size()));
    r.messages.reserve(layout.units().size() + 2);
    os::LargeVector<std::pair<size_t, size_t>> at;
    os::LargeVector<size_t> which;

    if (d != nullptr) {
        which.push_back(0);
        at.emplace_back(r.arena.size(), prompt.size());
        r.arena += prompt;
        r.messages.push_back({.role = net::LlmRole::System});
    }
    size_t bytes = 0;
    for (const Unit& u : layout.units()) {
        bytes += layout.bytes(u);
        layout.emit(u, r, at, which);
    }
    if (d != nullptr) {
        which.push_back(r.messages.size());
        at.emplace_back(r.arena.size(), info.size());
        r.arena += info;
        r.messages.push_back({.role = net::LlmRole::System});
        r.fixed_tokens = (prompt.size() + info.size()) / 3;
    }
    settle(r, at, which);
    r.est_tokens = r.fixed_tokens + bytes / 3;
    return r;
}

}  // namespace

BuiltRequest build_request(std::span<const Message> history, const DeviceSnapshot& d) {
    return build(history, &d);
}

BuiltRequest build_history(std::span<const Message> history) { return build(history, nullptr); }

bool needs_compression(size_t est_tokens, uint32_t window) {
    return uint64_t{est_tokens} * 100 > uint64_t{window} * kCompressPercent;
}

std::optional<std::pair<uint64_t, uint64_t>> pick_compression(std::span<const Message> history,
                                                              uint32_t window,
                                                              size_t fixed_tokens) {
    struct Group {
        uint64_t first = 0, last = 0;
        size_t bytes = 0;
        bool summary = false;
    };
    const Layout layout(history);
    os::LargeVector<Group> groups;
    uint32_t turn = 0;
    for (const Unit& u : layout.units()) {
        const bool join =
            !u.summary && !groups.empty() && !groups.back().summary && u.m->turn == turn;
        if (!join) groups.push_back({u.first, u.last, 0, u.summary});
        Group& g = groups.back();
        g.last = u.last;
        g.bytes += layout.bytes(u);
        if (!u.summary) turn = u.m->turn;
    }

    // The earliest of the last kKeepRecentTurns turns: never picked.
    size_t limit = groups.size();
    for (uint32_t kept = 0; limit > 0 && kept < kKeepRecentTurns;) {
        --limit;
        if (!groups[limit].summary) ++kept;
    }

    size_t rest = 0;
    for (const Group& g : groups) rest += g.bytes;
    auto fits = [&] {
        return (uint64_t{fixed_tokens} + rest / 3) * 100 <=
               uint64_t{window} * kCompressTargetPercent;
    };
    size_t n = 0;
    while (n < limit && !fits()) rest -= groups[n++].bytes;
    // Summarizing a lone summary again gains nothing.
    if (n == 0 || (n == 1 && groups[0].summary)) return std::nullopt;
    return std::make_pair(groups[0].first, groups[n - 1].last);
}

BuiltRequest build_summary_request(std::span<const Message> history, uint64_t first_seq,
                                   uint64_t last_seq) {
    BuiltRequest r;
    const Layout layout(history);
    r.arena.reserve(4096);
    for (const Unit& u : layout.units()) {
        if (u.first < first_seq || u.last > last_seq) continue;
        const Message& m = *u.m;
        auto line = [&](std::string_view lead, std::string_view text) {
            if (!r.arena.empty()) r.arena += '\n';
            r.arena.append(lead.data(), lead.size());
            r.arena.append(text.data(), text.size());
        };
        if (u.summary) {
            if (!m.text.empty()) line("Earlier summary: ", sv(m.text));
            continue;
        }
        switch (m.role) {
            case Role::User:
                line("User: ", sv(m.text));
                break;
            case Role::Assistant:
                if (!m.text.empty()) line("Assistant: ", sv(m.text));
                for (const ToolCallRec& c : m.calls) {
                    line("Assistant called ", wire_name(sv(c.name)));
                    r.arena += '(';
                    r.arena += c.args;
                    r.arena += ')';
                }
                break;
            case Role::Tool: {
                const std::string_view t = sv(m.text);
                line("Result of " + wire_name(sv(m.tool)) + ": ",
                     t.substr(0, prefix_bytes(t, kToolResultKeepBytes)));
                if (t.size() > kToolResultKeepBytes) r.arena += cut_note(t.size());
                break;
            }
            case Role::Note:
                line("System note: ", sv(m.text));
                break;
            case Role::Summary:
            case Role::Marker:
                break;
        }
    }
    r.messages.push_back({.role = net::LlmRole::System, .text = kSummaryPrompt});
    r.messages.push_back({.role = net::LlmRole::User, .text = std::string_view(r.arena)});
    r.est_tokens = (kSummaryPrompt.size() + r.arena.size()) / 3;
    return r;
}

}  // namespace wisp::agent
