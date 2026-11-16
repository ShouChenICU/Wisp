// The history part of a model request, and compression
// (wisp/agent/context.hpp).
//
// The history as the model sees it is a list of units: each message that
// goes on the wire, with every live summary standing in once for the
// messages it covers, at the place of the first of them. A summary is live
// unless a later one covers all it covers (a summary folded into a newer
// one); an empty summary covers turns dropped without one. A carry (the
// summary a rollover kept, spec 3.8) is a summary unit of its own seq,
// with its own lead; a later summary whose range takes in its seq hides it
// like any covered message. Notes and
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
constexpr std::string_view kCarryLead = "Summary of the previous conversation:\n";

constexpr std::string_view kSummaryPrompt =
    "Summarize the conversation transcript in the next message for your own use later in the "
    "same conversation: the transcript will be dropped and only your summary kept. Keep what "
    "still matters: what the user asked for and decided, their preferences, the scenes created "
    "or changed (ids, files, versions), problems left open and anything promised. Leave out "
    "greetings and tool output that no longer matters. Write plain sentences in the language of "
    "the conversation, at most 300 words, without <say> tags or Markdown.";

constexpr std::string_view kCarryPrompt =
    "Summarize the conversation transcript in the next message for a new conversation that starts "
    "from this summary: the transcript will be archived and only your summary carried over. Keep "
    "what still matters: what the user asked for and decided, their preferences, the scenes "
    "created or changed (ids, files, versions), problems left open and anything promised. Leave "
    "out greetings and tool output that no longer matters. Write plain sentences in the language "
    "of the conversation, at most 300 words, without <say> tags or Markdown.";

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

// The running turn's results are cut only when they do not fit; the model
// is still working from them, so the note says how to get the rest.
std::string live_cut_note(size_t total) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "\n[truncated; read in smaller pieces, %zu bytes total]", total);
    return buf;
}

// What a large call argument of an earlier turn is sent as.
std::string elided_args_text(size_t bytes) {
    return R"({"elided_bytes":)" + std::to_string(bytes) + "}";
}

struct Unit {
    const Message* m = nullptr;
    bool summary = false;          // a live summary or the carry
    uint64_t first = 0, last = 0;  // the seqs it stands for
};

std::string_view lead_of(const Message& m) {
    return m.role == Role::Carry ? kCarryLead : kSummaryLead;
}

bool in_sorted(const os::LargeVector<std::string_view>& v, std::string_view id) {
    return std::binary_search(v.begin(), v.end(), id);
}

class Layout {
public:
    explicit Layout(std::span<const Message> h, size_t live_cap = kLiveToolResultBytes,
                    std::span<const ReasoningRef> reasoning = {})
        : live_turn_(h.empty() ? 0 : h.back().turn), live_cap_(live_cap), reasoning_(reasoning) {
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
            units_.push_back({&m, m.role == Role::Carry, m.seq, m.seq});
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
        if (u.summary) return m.text.empty() ? 0 : lead_of(m).size() + m.text.size();
        switch (m.role) {
            case Role::User:
                return m.text.size();
            case Role::Assistant: {
                size_t n = m.text.size();
                bool any_call = false;
                for (const ToolCallRec& c : m.calls) {
                    if (!kept(c)) continue;
                    any_call = true;
                    n += c.id.size() + c.name.size() + args_bytes(m, c);
                }
                // An assistant message that sends nothing carries no reasoning.
                if (!m.text.empty() || any_call) n += reasoning_for(m.seq).size();
                return n;
            }
            case Role::Tool:
                if (!in_sorted(asked_, sv(m.call_id))) return 0;
                return m.call_id.size() + tool_text_bytes(m);
            case Role::Note:
            case Role::Summary:
            case Role::Marker:
            case Role::Carry:
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
            if (!m.text.empty())
                r.messages[arena_text(net::LlmRole::System, lead_of(m), sv(m.text))].movable = true;
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
                                     .arguments = call_args(m, c)});
                }
                if (m.text.empty() && calls.empty()) break;
                r.calls.push_back(std::move(calls));
                net::LlmMessage& out = r.messages.emplace_back();
                out.role = net::LlmRole::Assistant;
                out.text = sv(m.text);
                // The inner vector's buffer stays put when `calls` grows.
                out.tool_calls = {r.calls.back().data(), r.calls.back().size()};
                out.reasoning = reasoning_for(m.seq);
                break;
            }
            case Role::Tool: {
                if (!in_sorted(asked_, sv(m.call_id))) break;
                size_t idx;
                const size_t cap = tool_cap(m);
                if (m.text.size() <= cap) {
                    net::LlmMessage& out = r.messages.emplace_back();
                    out.role = net::LlmRole::Tool;
                    out.text = sv(m.text);
                    idx = r.messages.size() - 1;
                } else {
                    const std::string_view t = sv(m.text);
                    idx = arena_text(net::LlmRole::Tool, t.substr(0, prefix_bytes(t, cap)),
                                     note_for(m));
                }
                r.messages[idx].tool_call_id = sv(m.call_id);
                break;
            }
            case Role::Note:
            case Role::Summary:
            case Role::Marker:
            case Role::Carry:
                break;
        }
    }

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);

    // The kept reasoning of the assistant message `seq`, or "".
    std::string_view reasoning_for(uint64_t seq) const {
        auto it = std::lower_bound(reasoning_.begin(), reasoning_.end(), seq,
                                   [](const ReasoningRef& r, uint64_t s) { return r.seq < s; });
        return it != reasoning_.end() && it->seq == seq ? it->text : std::string_view{};
    }

    // The turn in progress gets its results whole: the model has not seen
    // them yet, or is still working from them. Earlier turns keep a short cut.
    size_t tool_cap(const Message& m) const {
        return m.turn == live_turn_ ? live_cap_ : kToolResultKeepBytes;
    }

    std::string note_for(const Message& m) const {
        return m.turn == live_turn_ ? live_cut_note(m.text.size()) : cut_note(m.text.size());
    }

    bool elides(const Message& m, const ToolCallRec& c) const {
        return m.turn != live_turn_ && c.args.size() > kCallArgsKeepBytes;
    }

    size_t args_bytes(const Message& m, const ToolCallRec& c) const {
        return elides(m, c) ? elided_args_text(c.args.size()).size() : c.args.size();
    }

    os::LargeString call_args(const Message& m, const ToolCallRec& c) const {
        if (!elides(m, c)) return c.args;
        const std::string e = elided_args_text(c.args.size());
        return os::LargeString(e.data(), e.size());
    }

    size_t tool_text_bytes(const Message& m) const {
        const size_t cap = tool_cap(m);
        if (m.text.size() <= cap) return m.text.size();
        return prefix_bytes(sv(m.text), cap) + note_for(m).size();
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

    uint32_t live_turn_;                       // turn of the last message
    size_t live_cap_;                          // its tool results' cut
    std::span<const ReasoningRef> reasoning_;  // by seq
    os::LargeVector<const Message*> live_;     // by covers_first
    os::LargeVector<Unit> units_;
    os::LargeVector<std::string_view> answered_, asked_;
};

// Points the arena messages at their text, now that the arena is final.
void settle(BuiltRequest& r, const os::LargeVector<std::pair<size_t, size_t>>& at,
            const os::LargeVector<size_t>& which) {
    for (size_t i = 0; i < which.size(); ++i)
        r.messages[which[i]].text = std::string_view(r.arena).substr(at[i].first, at[i].second);
}

BuiltRequest build(std::span<const Message> history, const DeviceSnapshot* d, BuildOptions opts) {
    BuiltRequest r;
    const Layout layout(history, opts.live_tool_bytes, opts.reasoning);
    std::string prompt, info;
    if (d != nullptr) {
        prompt = system_prompt(*d, opts.system_msgs);
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
        r.messages.push_back({.role = net::LlmRole::System, .movable = true});
        r.fixed_tokens = (prompt.size() + info.size()) / 3;
    }
    settle(r, at, which);
    r.est_tokens = r.fixed_tokens + bytes / 3;
    return r;
}

}  // namespace

BuiltRequest build_request(std::span<const Message> history, const DeviceSnapshot& d,
                           BuildOptions opts) {
    return build(history, &d, opts);
}

BuiltRequest build_history(std::span<const Message> history, BuildOptions opts) {
    return build(history, nullptr, opts);
}

bool needs_compression(size_t est_tokens, uint32_t window) {
    return uint64_t{est_tokens} * 100 > uint64_t{window} * kCompressPercent;
}

std::optional<std::pair<uint64_t, uint64_t>> pick_compression(std::span<const Message> history,
                                                              uint32_t window, size_t fixed_tokens,
                                                              uint32_t keep_recent) {
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

    // The earliest of the last `keep_recent` turns: never picked. The
    // running turn is always among them.
    size_t limit = groups.size();
    for (uint32_t kept = 0; limit > 0 && kept < std::max<uint32_t>(1, keep_recent);) {
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

namespace {

// Appends the unit's transcript lines to `out`, each after a '\n' unless
// `out` is empty: the summary and carry requests' shared format.
void transcript_lines(const Unit& u, os::LargeString& out) {
    const Message& m = *u.m;
    auto line = [&](std::string_view lead, std::string_view text) {
        if (!out.empty()) out += '\n';
        out.append(lead.data(), lead.size());
        out.append(text.data(), text.size());
    };
    if (u.summary) {
        if (!m.text.empty()) line("Earlier summary: ", sv(m.text));
        return;
    }
    switch (m.role) {
        case Role::User:
            line("User: ", sv(m.text));
            break;
        case Role::Assistant:
            if (!m.text.empty()) line("Assistant: ", sv(m.text));
            for (const ToolCallRec& c : m.calls) {
                line("Assistant called ", wire_name(sv(c.name)));
                out += '(';
                if (c.args.size() > kCallArgsKeepBytes)
                    out += elided_args_text(c.args.size());
                else
                    out += c.args;
                out += ')';
            }
            break;
        case Role::Tool: {
            const std::string_view t = sv(m.text);
            line("Result of " + wire_name(sv(m.tool)) + ": ",
                 t.substr(0, prefix_bytes(t, kToolResultKeepBytes)));
            if (t.size() > kToolResultKeepBytes) out += cut_note(t.size());
            break;
        }
        case Role::Note:
            line("System note: ", sv(m.text));
            break;
        case Role::Summary:
        case Role::Marker:
        case Role::Carry:
            break;
    }
}

}  // namespace

BuiltRequest build_summary_request(std::span<const Message> history, uint64_t first_seq,
                                   uint64_t last_seq) {
    BuiltRequest r;
    const Layout layout(history);
    r.arena.reserve(4096);
    for (const Unit& u : layout.units()) {
        if (u.first < first_seq || u.last > last_seq) continue;
        transcript_lines(u, r.arena);
    }
    r.messages.push_back({.role = net::LlmRole::System, .text = kSummaryPrompt});
    r.messages.push_back({.role = net::LlmRole::User, .text = std::string_view(r.arena)});
    r.est_tokens = (kSummaryPrompt.size() + r.arena.size()) / 3;
    return r;
}

BuiltRequest build_carry_request(std::span<const Message> history, size_t budget_tokens) {
    BuiltRequest r;
    const Layout layout(history);
    const os::LargeVector<Unit>& units = layout.units();
    for (const Unit& u : units)
        if (u.summary) transcript_lines(u, r.arena);

    // The newest whole turns, each rendered alone (newest first) until one
    // does not fit, then appended oldest first.
    const uint64_t budget_bytes = uint64_t{budget_tokens} * 3 + 2;
    uint64_t used = r.arena.size();
    os::LargeVector<os::LargeString> turns;
    size_t end = units.size();
    while (end > 0) {
        if (units[end - 1].summary) {
            --end;
            continue;
        }
        const uint32_t turn = units[end - 1].m->turn;
        size_t begin = end;
        while (begin > 0 && !units[begin - 1].summary && units[begin - 1].m->turn == turn) --begin;
        // Rendering stops as soon as the turn is past the budget, so a huge
        // turn is never held whole.
        os::LargeString piece;
        const auto grown = [&] { return used + piece.size() + (used > 0 ? 1 : 0); };
        for (size_t i = begin; i < end && grown() <= budget_bytes; ++i)
            transcript_lines(units[i], piece);
        end = begin;
        if (piece.empty()) continue;
        if (grown() > budget_bytes) break;
        used = grown();
        turns.push_back(std::move(piece));
    }
    r.arena.reserve(used);
    for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
        if (!r.arena.empty()) r.arena += '\n';
        r.arena += *it;
    }
    if (r.arena.empty()) return r;
    r.messages.push_back({.role = net::LlmRole::System, .text = kCarryPrompt});
    r.messages.push_back({.role = net::LlmRole::User, .text = std::string_view(r.arena)});
    r.est_tokens = (kCarryPrompt.size() + r.arena.size()) / 3;
    return r;
}

}  // namespace wisp::agent
