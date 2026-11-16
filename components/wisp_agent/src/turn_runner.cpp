#include "turn_runner.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "utf8.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kTag = "agent";

// Bytes in the longest prefix of `s` with at most `max_cp` code points.
size_t prefix_code_points(std::string_view s, size_t max_cp) {
    size_t cp = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (cp == max_cp) return i;
            ++cp;
        }
    }
    return s.size();
}

// A time limit in whole minutes for the note, at least 1.
uint64_t minutes(uint64_t ms) { return std::max<uint64_t>(1, (ms + 30000) / 60000); }

// The instruction after a reply cut by the length limit (spec 3.7).
constexpr std::string_view kContinuePrompt =
    "Your previous reply was cut off by the length limit. Continue exactly where it stopped. Do "
    "not repeat anything.";

}  // namespace

std::string summary(std::string_view s) {
    return std::string(s.substr(0, prefix_code_points(s, kSummaryChars)));
}

Outcome TurnRunner::run(const TurnInput& in) {
    in_ = in;
    Outcome out;
    keep_ = ReasoningKeep();
    images_.clear();
    // The provider's active model's context window, model, profile,
    // dialect and vision, read once per turn.
    model_ = llm_.active_model();
    const std::optional<net::LlmModelInfo>& m = model_;
    const uint32_t window = m ? m->context_window : kDefaultContextWindow;
    cal_.begin_turn(m ? std::string_view(m->profile_id) : std::string_view(),
                    m ? std::string_view(m->model) : std::string_view());
    out.status = loop(window, out);
    return out;
}

// Its usage neither counts in the turn's tokens nor calibrates: the turn
// has not started, and the carry request is not one the estimate made
// (spec 3.8).
std::optional<os::LargeString> TurnRunner::make_carry(const TurnInput& in,
                                                      std::span<const Message> history) {
    // request() takes the turn's limit from in_.
    in_ = in;
    model_ = llm_.active_model();
    if (!model_) {
        WLOG_I(kTag, "carry skipped: no_model");
        return std::nullopt;
    }
    if (!env_.network_up()) {
        WLOG_I(kTag, "carry skipped: network");
        return std::nullopt;
    }
    cal_.begin_turn(model_->profile_id, model_->model);
    // bytes / 3 like the window the summaries fill (see loop()).
    const size_t budget =
        size_t{cal_.unscaled_window(model_->context_window)} * kCarryInputPercent / 100;
    net::LlmResult res;
    {
        const BuiltRequest req = build_carry_request(history, budget);
        // The live summaries go in whole, ahead of the turns: past the
        // budget on their own, the request would be too.
        if (req.messages.empty() || req.arena.size() / 3 > budget) {
            WLOG_I(kTag, "carry skipped: empty");
            return std::nullopt;
        }
        env_.set_state(AgentState::Thinking);
        res = request({req.messages.data(), req.messages.size()}, {}, nullptr);
    }
    if (env_.aborted()) {
        WLOG_I(kTag, "carry skipped: %s", net::to_string(net::LlmError::Cancelled));
        return std::nullopt;
    }
    // A reply cut by the length limit is used as it is, like a summary's.
    if (!whole(res) || res.text.empty()) {
        WLOG_I(kTag, "carry skipped: %s", whole(res) ? "empty" : net::to_string(res.error));
        return std::nullopt;
    }
    res.text.resize(prefix_bytes(sv(res.text), kCarryMaxBytes));
    WLOG_I(kTag, "carry %zu bytes", res.text.size());
    return std::move(res.text);
}

// Where `out` is given, the note is also the turn's last text.
void TurnRunner::note(ui::StrId id, Outcome* out) {
    if (out == nullptr) {
        env_.note(id);
        return;
    }
    note_text(ui::tr(id, env_.lang()), out);
}

void TurnRunner::note_text(std::string_view text, Outcome* out) {
    if (out != nullptr) out->last_text.assign(text);
    env_.note_text(text);
}

const char* TurnRunner::finish_abort(Outcome& out) {
    switch (env_.abort_reason()) {
        case Abort::Time:
            note_text(ui::tr_fill(ui::StrId::AgentTimeLimit, env_.lang(),
                                  std::to_string(minutes(in_.turn_ms))),
                      &out);
            out.reason = "time_limit";
            return "time_limit";
        case Abort::Network:
        case Abort::Stall:
            env_.set_state(AgentState::Error);
            note(ui::StrId::AgentErrNetworkLost, &out);
            out.reason = "network_lost";
            return "error";
        case Abort::Stop:
        case Abort::None:
            break;
    }
    note(ui::StrId::AgentStopped, &out);
    out.reason = "stopped";
    return "stopped";
}

// Answers the calls from `from` on without running them, so the next
// request still pairs every call with a result.
void TurnRunner::skip_calls(std::span<const net::LlmToolCall> calls, std::string_view why) {
    for (const net::LlmToolCall& c : calls) {
        Message m;
        m.role = Role::Tool;
        m.turn = in_.turn;
        m.call_id.assign(c.id);
        const ToolDef* def = tools_.find_wire(c.name);
        m.tool.assign(def != nullptr ? std::string_view(def->name) : std::string_view(c.name));
        m.ok = false;
        m.text.assign(to_json(tool_error("cancelled", why).result));
        env_.append(std::move(m));
    }
}

BuiltRequest TurnRunner::build(std::span<const Message> h, BuildOptions opts) const {
    const core::LlmDialect dl = dialect();
    opts.system_msgs = dl.system_msgs;
    // Only what is sent counts in the estimate: without Echo the body
    // drops reasoning, so none is attached.
    if (dl.reasoning == core::LlmReasoning::Echo) opts.reasoning = keep_.refs();
    BuiltRequest req;
    if (!context_) {
        req = build_history(h, opts);
    } else {
        DeviceSnapshot d = context_(in_.source);
        d.source = in_.source;
        req = build_request(h, d, opts);
    }
    if (!images_.empty()) {
        // `list` may die with this frame: the request views only the mime
        // strings and the bytes, which images_ owns for the whole turn.
        os::LargeVector<RequestImage> list;
        list.reserve(images_.size());
        for (const TurnImage& t : images_) {
            RequestImage& ri = list.emplace_back();
            ri.tool = sv(t.tool);
            ri.call_id = sv(t.call_id);
            if (t.image) {
                ri.mime = t.image->mime;
                ri.bytes = {t.image->bytes.data(), t.image->bytes.size()};
            }
        }
        attach_images(req, {list.data(), list.size()});
    }
    return req;
}

// One model request under the turn's controls: the user's stop, the
// network, the stall check and the time limit.
net::LlmResult TurnRunner::request(std::span<const net::LlmMessage> messages,
                                   std::string_view tools_json,
                                   std::function<void(std::string_view)> on_text) {
    TurnControls& ctl = env_.controls();
    const uint64_t spent = env_.now_ms() - in_.start_ms;
    const uint64_t limit = in_.turn_ms;
    const uint64_t left = spent >= limit ? 1 : limit - spent;
    net::LlmCall call;
    call.messages = messages;
    call.tools_json = tools_json;
    // Past the turn's deadline by a margin, so the time limit (checked
    // every kCheckMs) ends the request, not its own deadline, which
    // would read as a transport error.
    call.timeout_ms = static_cast<uint32_t>(
        std::min<uint64_t>(left + AgentService::kDeadlineMarginMs, AgentService::kRequestMs));
    call.watch = ctl.watch;
    call.on_armed = [this, &ctl](uint32_t t) {
        ctl.ticket = t;
        if (env_.abort_reason() != Abort::None) ctl.watch->abort(t);
    };
    call.on_text = std::move(on_text);
    call.on_bytes = [this, &ctl] { ctl.last_byte = env_.now_ms(); };
    call.cancelled = [this] { return env_.aborted(); };
    call.on_tool = [this](const net::LlmToolProgress& p) {
        const ToolDef* def = tools_.find_wire(p.name);
        AgentEvent e;
        e.kind = AgentEvent::Kind::ToolPrep;
        e.tool.assign(def != nullptr ? std::string_view(def->name) : p.name);
        e.bytes = static_cast<uint32_t>(std::min<size_t>(p.arg_bytes, UINT32_MAX));
        env_.emit(e);
    };
    ctl.last_byte = env_.now_ms();
    ctl.in_request = true;
    net::LlmResult res = llm_.complete(call);
    ctl.in_request = false;
    return res;
}

// The request that was sent, then the cut reply and the instruction to go
// on (spec 3.7); neither of the two is stored. The request is reused as it
// is, not rebuilt or compressed again.
net::LlmResult TurnRunner::continue_reply(std::span<const net::LlmMessage> sent,
                                          std::string_view tools_json, const net::LlmResult& cut,
                                          std::function<void(std::string_view)> on_text) {
    os::LargeVector<net::LlmMessage> msgs;
    msgs.reserve(sent.size() + 2);
    msgs.assign(sent.begin(), sent.end());
    net::LlmMessage partial;
    partial.role = net::LlmRole::Assistant;
    partial.text = sv(cut.text);
    // A cut reasoning block is never echoed (spec 3.3).
    if (cut.error == net::LlmError::None && dialect().reasoning == core::LlmReasoning::Echo)
        partial.reasoning = sv(cut.reasoning);
    msgs.push_back(partial);
    net::LlmMessage go_on;
    go_on.role = net::LlmRole::User;
    go_on.text = kContinuePrompt;
    msgs.push_back(go_on);
    return request({msgs.data(), msgs.size()}, tools_json, std::move(on_text));
}

// A connection dropped after finish_reason but before [DONE] still
// delivered the whole reply.
bool TurnRunner::whole(const net::LlmResult& res) {
    const bool finished = res.finish == net::LlmFinish::Stop ||
                          res.finish == net::LlmFinish::ToolCalls ||
                          res.finish == net::LlmFinish::Length;
    return res.error == net::LlmError::None || (res.error == net::LlmError::Transport && finished);
}

// Summarizes history[first..last] and stores the summary, or, when the
// model fails to give one, stores an empty one (the turns are dropped
// from the context; the file keeps them) and says so. Without a model,
// with a refused key or without a connection nothing is stored: the
// request that follows fails the same way and explains it, and the
// turns stay for a later attempt (Kept).
TurnRunner::Compressed TurnRunner::compress(std::span<const Message> h,
                                            std::pair<uint64_t, uint64_t> range, Outcome& out) {
    net::LlmResult res;
    {
        const BuiltRequest req = build_summary_request(h, range.first, range.second);
        res = request({req.messages.data(), req.messages.size()}, {}, nullptr);
    }
    if (res.usage) out.tokens += res.usage->total;
    if (env_.aborted()) return Compressed::Aborted;
    const bool ok = whole(res) && !res.text.empty();
    if (!ok && (res.error == net::LlmError::NoModel || res.error == net::LlmError::Unauthorized ||
                res.error == net::LlmError::Transport)) {
        WLOG_W(kTag, "context not compressed: %s", net::to_string(res.error));
        return Compressed::Kept;
    }
    WLOG_I(kTag, "context %s seq %llu-%llu", ok ? "summarized" : "dropped",
           static_cast<unsigned long long>(range.first),
           static_cast<unsigned long long>(range.second));
    Message m;
    m.role = Role::Summary;
    m.turn = in_.turn;
    m.covers_first = range.first;
    m.covers_last = range.second;
    if (ok) m.text = std::move(res.text);
    env_.append(std::move(m));
    if (!ok) note(ui::StrId::AgentContextDropped);
    return Compressed::Stored;
}

const char* TurnRunner::loop(uint32_t window, Outcome& out) {
    const uint32_t turn = in_.turn;
    uint32_t generated = 0;
    // Set once the context has been compressed for the coming request
    // (first sparing the last kKeepRecentTurns turns, then only the
    // running one), so a history still too long after that is sent as
    // it is, with the running turn's results cut.
    bool compressed = false;
    bool compressed_deep = false;
    // The model refused the last request as too long: the next one is
    // treated as over the window whatever the estimate says. Once per
    // turn.
    bool overflow = false;
    bool overflow_retried = false;
    // The refused request's estimate while its retry is being
    // prepared: a retry that did not shrink is the same request.
    std::optional<size_t> refused_tokens;
    const std::string_view tools_json = sv(tools_.tools_json(vision()));
    const size_t tools_tokens = tools_json.size() / 3;
    for (;;) {
        if (env_.aborted()) return finish_abort(out);
        if (!env_.network_up()) {
            env_.set_state(AgentState::Error);
            note(ui::StrId::AgentErrNetwork, &out);
            out.reason = "network";
            return "error";
        }
        env_.set_state(AgentState::Thinking);

        net::LlmResult res;
        // The continuation of a reply cut by the length limit, once per
        // reply (spec 3.7).
        std::optional<net::LlmResult> cont;
        size_t sent_tokens = 0;
        {
            const std::shared_ptr<const History> hist = store_.history();
            BuiltRequest req = build(*hist);
            const bool over =
                overflow || needs_compression(cal_.scaled(req.est_tokens + tools_tokens), window);
            if (over && !compressed_deep) {
                // pick_compression() counts bytes / 3: the window it
                // fills is the real one unscaled.
                const uint32_t est_window = cal_.unscaled_window(window);
                const size_t fixed = req.fixed_tokens + tools_tokens;
                std::optional<std::pair<uint64_t, uint64_t>> range;
                if (!compressed) range = pick_compression(*hist, est_window, fixed);
                compressed = true;
                if (!range) {
                    compressed_deep = true;
                    range = pick_compression(*hist, est_window, fixed, 1);
                }
                if (range) {
                    const Compressed c = compress(*hist, *range, out);
                    if (c == Compressed::Aborted) return finish_abort(out);
                    // A summary request that failed this way fails the
                    // same way again: go on without summarizing.
                    if (c == Compressed::Kept) compressed_deep = true;
                    // Rebuilt with the summary in place, and judged by
                    // the estimate again.
                    if (c == Compressed::Stored) overflow = false;
                    continue;
                }
            }
            if (overflow ||
                uint64_t{cal_.scaled(req.est_tokens + tools_tokens)} > uint64_t{window}) {
                // Nothing more to summarize: the running turn's own
                // results are what does not fit.
                WLOG_W(kTag, "context over the window: cutting this turn's tool results");
                req = build(*hist, {.live_tool_bytes = kToolResultKeepBytes});
            }
            compressed = false;
            compressed_deep = false;
            overflow = false;
            sent_tokens = req.est_tokens + tools_tokens;
            if (refused_tokens && sent_tokens >= *refused_tokens) {
                // Nothing was summarized or cut: the model would refuse
                // it again.
                env_.set_state(AgentState::Error);
                note(ui::StrId::AgentErrContext, &out);
                out.reason = "context";
                return "error";
            }
            refused_tokens.reset();
            bool output = false;
            // Shared by the continuation: its deltas go on with the same
            // text, and Output is set once.
            const auto on_text = [&](std::string_view d) {
                if (!output) {
                    output = true;
                    env_.set_state(AgentState::Output);
                }
                AgentEvent e;
                e.kind = AgentEvent::Kind::Delta;
                e.text.assign(d);
                env_.emit(e);
            };
            const std::span<const net::LlmMessage> sent(req.messages.data(), req.messages.size());
            res = request(sent, tools_json, on_text);
            // An empty cut reply has nothing to go on from; with tool calls
            // the bad_args rule below applies instead; after a stop the
            // partial text is stored and the turn ends.
            if (whole(res) && res.finish == net::LlmFinish::Length && res.tool_calls.empty() &&
                !res.text.empty() && !env_.aborted())
                cont = continue_reply(sent, tools_json, res, on_text);
        }
        if (res.usage) out.tokens += res.usage->total;
        // Only the first request's usage calibrates: the continuation's
        // request is not the one estimated.
        if (whole(res) && res.usage && res.usage->prompt > 0) {
            cal_.observe(res.profile_id, res.model, res.usage->prompt, sent_tokens);
        }
        // A reply's reasoning is echoed only from an attempt without an
        // error (a cut reasoning block never is).
        bool echo_reasoning = res.error == net::LlmError::None;
        if (cont) {
            if (cont->usage) out.tokens += cont->usage->total;
            // The reply is the cut text followed by what came of the
            // continuation, so far as it got. Only a whole continuation
            // brings its calls and its finish: a failed one leaves the
            // reply cut (finish Length), which the note below reports.
            // The joined reasoning is echoed only when the first part was
            // whole: the continuation's alone would echo half a block.
            res.text.append(cont->text.data(), cont->text.size());
            if (!echo_reasoning) res.reasoning.clear();
            if (whole(*cont)) {
                if (echo_reasoning && cont->error == net::LlmError::None)
                    res.reasoning.append(cont->reasoning.data(), cont->reasoning.size());
                res.tool_calls = std::move(cont->tool_calls);
                res.finish = cont->finish;
            }
        }

        if (!whole(res)) {
            if (!res.text.empty()) {
                Message m;
                m.role = Role::Assistant;
                m.turn = turn;
                m.text = res.text;
                env_.append(std::move(m));
            }
            // aborted() also checks the time limit: a request that ran
            // into it ends as time_limit, not as an error.
            if (env_.aborted()) return finish_abort(out);
            if (res.error == net::LlmError::ContextLength && !overflow_retried) {
                // Our estimate was short of the model's count: trust it
                // less from now on, compress harder and try once more.
                overflow_retried = true;
                overflow = true;
                refused_tokens = sent_tokens;
                cal_.overflow(res.profile_id, res.model);
                WLOG_W(kTag, "context too long for the model: compressing and retrying");
                continue;
            }
            struct Why {
                ui::StrId id;
                const char* reason;
            };
            Why why{ui::StrId::AgentErrServer, "server"};
            if (res.error == net::LlmError::ContextLength)
                why = {ui::StrId::AgentErrContext, "context"};
            if (res.error == net::LlmError::NoModel) why = {ui::StrId::AgentErrNoModel, "no_model"};
            if (res.error == net::LlmError::Unauthorized)
                why = {ui::StrId::AgentErrUnauthorized, "unauthorized"};
            if (res.error == net::LlmError::Transport)
                why = res.any_body ? Why{ui::StrId::AgentErrNetworkLost, "network_lost"}
                                   : Why{ui::StrId::AgentErrNetwork, "network"};
            env_.set_state(AgentState::Error);
            note(why.id, &out);
            out.reason = why.reason;
            return "error";
        }

        Message reply;
        reply.role = Role::Assistant;
        reply.turn = turn;
        reply.text = res.text;
        for (net::LlmToolCall& c : res.tool_calls) {
            // Every call needs an id for its result to refer to.
            // The turn keeps the id unique in the whole conversation,
            // where the request builder pairs calls with results.
            if (c.id.empty())
                c.id = "call_" + std::to_string(turn) + "_" + std::to_string(++generated);
            const ToolDef* def = tools_.find_wire(c.name);
            ToolCallRec rec;
            rec.id.assign(c.id);
            rec.name.assign(def != nullptr ? std::string_view(def->name)
                                           : std::string_view(c.name));
            rec.args = c.arguments;
            reply.calls.push_back(std::move(rec));
        }
        if (!res.text.empty()) out.last_text = res.text;
        if (!reply.text.empty() || !reply.calls.empty()) {
            const uint64_t seq = env_.append(std::move(reply));
            // A reply whose connection dropped after finish_reason is
            // whole enough to store, but its reasoning may be cut, and a
            // cut reasoning block is never echoed.
            if (echo_reasoning && dialect().reasoning == core::LlmReasoning::Echo)
                keep_.add(seq, sv(res.reasoning));
        }
        if (res.tool_calls.empty()) {
            if (env_.aborted()) return finish_abort(out);
            // Still cut after the continuation, or cut with nothing to
            // continue: said in a note that leaves the reply as the done
            // notice's text (spec 3.7).
            if (res.finish == net::LlmFinish::Length) note(ui::StrId::AgentReplyCut);
            return "done";
        }

        const std::span<const net::LlmToolCall> calls(res.tool_calls);
        for (size_t i = 0; i < calls.size(); ++i) {
            if (env_.aborted()) {
                skip_calls(calls.subspan(i), "the turn was stopped");
                return finish_abort(out);
            }
            if (out.steps >= in_.max_steps) {
                skip_calls(calls.subspan(i), "the turn reached its step limit");
                note(ui::StrId::AgentStepLimit, &out);
                out.reason = "step_limit";
                return "step_limit";
            }
            // With finish_reason "length" the last call's arguments
            // may be cut anywhere: never hand them to the tool.
            const bool cut = res.finish == net::LlmFinish::Length && i + 1 == calls.size();
            run_tool(calls[i], cut, out);
        }
    }
}

void TurnRunner::run_tool(const net::LlmToolCall& c, bool cut, Outcome& out) {
    const ToolDef* def = tools_.find_wire(c.name);
    const std::string dotted = def != nullptr ? std::string(sv(def->name)) : c.name;
    env_.set_state(AgentState::Tool, dotted);
    AgentEvent start;
    start.kind = AgentEvent::Kind::ToolStart;
    start.tool = dotted;
    start.args = summary(sv(c.arguments));
    env_.emit(start);

    const uint64_t t0 = env_.now_ms();
    ToolOutput o;
    if (cut) {
        o = tool_error("bad_args", "the arguments were cut off (finish_reason length)",
                       Map{{"field", ""}});
    } else {
        const uint64_t spent = env_.now_ms() - in_.start_ms;
        const uint64_t limit = in_.turn_ms;
        const uint64_t left = spent >= limit ? 0 : limit - spent;
        ToolCtx ctx;
        ctx.cancelled = [this] { return env_.aborted(); };
        ctx.deadline_ms = os::now_ms() + left;
        ctx.lang = ui::lang_code(env_.lang());
        ctx.turn = in_.serial;
        ctx.vision = vision();
        o = tools_.call(c.name, sv(c.arguments), ctx);
    }
    // Only the newest image is kept whole (spec 3.5): the previous one's
    // bytes are freed now, and its request message says a newer one was
    // taken. Without vision, or from a failed call, the image is dropped.
    if (o.ok && o.image && vision()) {
        if (!images_.empty()) images_.back().image.reset();
        TurnImage& t = images_.emplace_back();
        t.tool.assign(dotted);
        t.call_id.assign(c.id);
        t.image = std::move(o.image);
    }
    o.image.reset();
    ++out.steps;
    const auto ms = static_cast<unsigned>(env_.now_ms() - t0);

    // A successful step the system tells the user about (spec 6.2):
    // the model learns it was told from the result itself, so the
    // history gains no extra system or user message.
    const bool noted = o.ok && !o.note.empty();
    if (noted && !o.result_json.empty()) {
        os::LargeString fields;
        fields.reserve(o.result_json.size() + o.note.size() + 32);
        const std::string_view json(o.result_json.data(), o.result_json.size());
        if (json.size() >= 2 && json.front() == '{' && json.back() == '}') {
            fields.append(json.data(), json.size() - 1);
            if (fields.size() > 1) fields.push_back(',');
            append_json_string(fields, "system_note");
            fields.push_back(':');
            append_json_string(fields, o.note);
            fields.push_back('}');
            o.result_json = std::move(fields);
        }
    } else if (noted && o.result.type() == Value::Type::Map) {
        Map fields = o.result.as_map();
        fields["system_note"] = Value(o.note);
        o.result = Value(std::move(fields));
    }

    Message m;
    m.role = Role::Tool;
    m.turn = in_.turn;
    m.call_id.assign(c.id);
    m.tool.assign(dotted);
    m.ok = o.ok;
    {
        if (!o.result_json.empty()) {
            m.text.assign(o.result_json.data(), o.result_json.size());
        } else {
            const std::string json = to_json(o.result);
            m.text.assign(json);
        }
    }
    std::string code;
    if (!o.ok) {
        const Value* err = o.result.find("error");
        code = err != nullptr && err->type() == Value::Type::String ? err->as_string() : "failed";
    }
    // The model chose the name: only registered names go into the log.
    const char* logged = def != nullptr ? def->name.c_str() : "unknown";
    if (o.ok) {
        WLOG_I(kTag, "tool %s ok %u ms", logged, ms);
    } else {
        WLOG_I(kTag, "tool %s error:%s %u ms", logged, code.c_str(), ms);
    }
    AgentEvent end;
    end.kind = AgentEvent::Kind::ToolEnd;
    end.tool = dotted;
    end.ok = o.ok;
    end.result = summary(sv(m.text));
    env_.append(std::move(m));
    if (noted) {
        Message n;
        n.role = Role::Note;
        n.turn = in_.turn;
        n.text.assign(o.note);
        env_.append(std::move(n));
    }
    env_.emit(end);
}

}  // namespace wisp::agent
