#include "wisp/shell/history.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/shell/body_text.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/symbols.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme.hpp"
#include "wisp/ui/theme_tokens.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/voice/say_parser.hpp"

namespace wisp::shell {

namespace {

constexpr const char* kTag = "shell";

std::string_view view_of(const os::LargeString& s) { return {s.data(), s.size()}; }

void append_line(os::LargeString& out, std::string_view text) {
    if (text.empty()) return;
    if (!out.empty() && out.back() != '\n') out += '\n';
    out.append(text.data(), text.size());
}

// Ends the turn being gathered: its reply split into say and body, its
// notes after the body.
void close_turn(os::LargeVector<HistoryTurn>& out, HistoryTurn& t, os::LargeString& reply,
                os::LargeString& notes) {
    if (!reply.empty()) {
        const voice::SaySplit split = voice::split_say(view_of(reply));
        t.say.assign(split.say.data(), split.say.size());
        t.body.assign(split.body.data(), split.body.size());
    }
    append_line(t.body, view_of(notes));
    if (!t.heard.empty() || !t.say.empty() || !t.body.empty()) out.push_back(std::move(t));
    t = HistoryTurn{};
    reply.clear();
    notes.clear();
}

}  // namespace

os::LargeVector<HistoryTurn> group_turns(std::span<const agent::Message> msgs) {
    os::LargeVector<HistoryTurn> out;
    HistoryTurn cur;
    os::LargeString reply, notes;
    bool open = false;
    for (const agent::Message& m : msgs) {
        if (m.role == agent::Role::Marker || m.role == agent::Role::Summary ||
            m.role == agent::Role::Tool)
            continue;
        if (!open || m.turn != cur.turn) {
            if (open) close_turn(out, cur, reply, notes);
            cur.turn = m.turn;
            open = true;
        }
        switch (m.role) {
            case agent::Role::User:
                cur.source = m.source;
                append_line(cur.heard, view_of(m.text));
                break;
            case agent::Role::Assistant:
                reply.append(m.text.data(), m.text.size());
                break;
            case agent::Role::Note:
                append_line(notes, view_of(m.text));
                break;
            default:
                break;
        }
    }
    if (open) close_turn(out, cur, reply, notes);
    return out;
}

HistoryPage::HistoryPage(HistoryDeps deps) : deps_(std::move(deps)) {}

HistoryPage::~HistoryPage() {
    *alive_ = false;
    if (busy_timer_ != nullptr) lv_timer_delete(busy_timer_);
}

void HistoryPage::build(lv_obj_t* list) {
    list_ = list;
    lv_obj_add_event_cb(list_, &HistoryPage::list_cb, LV_EVENT_SCROLL, this);
    rebuild();
}

void HistoryPage::rebuild() {
    detail::clear_rows(list_);
    cards_.clear();
    empty_ = busy_ = new_btn_ = nullptr;
    turns_.clear();
    if (deps_.agent != nullptr) {
        const agent::ConversationView v = deps_.agent->view();
        if (v.messages) turns_ = group_turns(*v.messages);
    }
    first_ = turns_.size() > kHistoryBatch ? turns_.size() - kHistoryBatch : 0;

    if (turns_.empty()) {
        empty_ = detail::make_text(list_, ui::tr(ui::StrId::HistoryEmpty), true);
        lv_obj_set_style_text_align(empty_, LV_TEXT_ALIGN_CENTER, 0);
    }
    busy_ = detail::make_text(list_, ui::tr(ui::StrId::HistoryBusy), true);
    lv_obj_set_style_text_align(busy_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(busy_, LV_OBJ_FLAG_HIDDEN);

    new_btn_ = lv_button_create(list_);
    lv_obj_set_size(new_btn_, LV_PCT(100), detail::kMinTouchPx);
    ui::set_button_role(new_btn_, ui::ButtonRole::Secondary);
    lv_obj_t* label = lv_label_create(new_btn_);
    lv_label_set_text(label, ui::tr(ui::StrId::HistoryNew));
    lv_obj_center(label);
    lv_obj_add_event_cb(new_btn_, &HistoryPage::new_cb, LV_EVENT_CLICKED, this);

    add_cards(first_, turns_.size());
    detail::bubble_tree(list_);
    lv_obj_update_layout(list_);
    lv_obj_scroll_to_y(list_, LV_COORD_MAX, LV_ANIM_OFF);
}

void HistoryPage::add_cards(size_t from, size_t to) {
    os::LargeVector<Card> made;
    made.reserve(to - from);
    for (size_t i = from; i < to; ++i) {
        const HistoryTurn& t = turns_[i];
        Card c;
        c.card = detail::make_card(list_);
        lv_obj_set_style_min_height(c.card, detail::kMinTouchPx, 0);
        lv_obj_add_flag(c.card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c.card, &HistoryPage::card_cb, LV_EVENT_CLICKED, this);
        // Under the title, above the busy line, the button and the cards
        // already there.
        lv_obj_move_to_index(c.card, detail::kFirstRow + static_cast<int32_t>(i - from));

        if (!t.heard.empty()) {
            os::LargeString heard;
            if (t.source == agent::Source::Voice) heard = WISP_SYMBOL_MICROPHONE " ";
            heard += t.heard;
            c.heard = detail::make_text(c.card, heard.c_str(), true);
        }
        if (!t.say.empty()) c.say = detail::make_text(c.card, t.say.c_str());
        if (!t.body.empty()) {
            os::LargeString fold = ui::tr(ui::StrId::HistoryMore);
            fold += " " LV_SYMBOL_DOWN;
            c.fold = detail::make_text(c.card, fold.c_str(), true);
        }
        made.push_back(c);
    }
    cards_.insert(cards_.begin(), made.begin(), made.end());
}

lv_obj_t* HistoryPage::card(size_t i) const { return i < cards_.size() ? cards_[i].card : nullptr; }
lv_obj_t* HistoryPage::heard_label(size_t i) const {
    return i < cards_.size() ? cards_[i].heard : nullptr;
}
lv_obj_t* HistoryPage::say_label(size_t i) const {
    return i < cards_.size() ? cards_[i].say : nullptr;
}
lv_obj_t* HistoryPage::fold_label(size_t i) const {
    return i < cards_.size() ? cards_[i].fold : nullptr;
}
lv_obj_t* HistoryPage::body_box(size_t i) const {
    return i < cards_.size() ? cards_[i].body : nullptr;
}
bool HistoryPage::expanded(size_t i) const { return i < cards_.size() && cards_[i].open; }

bool HistoryPage::at_bottom() const {
    return list_ == nullptr || lv_obj_get_scroll_bottom(list_) <= 0;
}

void HistoryPage::toggle(size_t i) {
    if (i >= cards_.size()) return;
    Card& c = cards_[i];
    const HistoryTurn& t = turns_[first_ + i];
    if (c.fold == nullptr) return;
    c.open = !c.open;
    if (c.body == nullptr) {
        // Built on the first unfold: most bodies are never read here.
        c.body = detail::make_box(c.card);
        lv_obj_set_flex_flow(c.body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c.body, ui::theme::kGapSm, 0);
        const bool colour = detail::colour_panel(c.card);
        for (const BodyBlock& b : light_markdown(view_of(t.body))) {
            lv_obj_t* l =
                detail::make_text(c.body, b.code ? code_fold_text(b).c_str() : b.text.c_str());
            if (!b.code) continue;
            lv_obj_set_style_pad_all(l, ui::theme::kGapSm, 0);
            lv_obj_set_style_radius(l, ui::theme::kRadiusSm, 0);
            if (colour) {
                lv_obj_set_style_bg_color(l, lv_color_hex(ui::theme::kMutedSoft), 0);
                lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_border_color(l, detail::text_colour(l), 0);
                lv_obj_set_style_border_width(l, ui::theme::kBorderWidth, 0);
            }
        }
        detail::bubble_tree(c.body);
    }
    if (c.open) {
        lv_obj_remove_flag(c.body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c.body, LV_OBJ_FLAG_HIDDEN);
    }
    os::LargeString fold = ui::tr(c.open ? ui::StrId::HistoryLess : ui::StrId::HistoryMore);
    fold += c.open ? " " LV_SYMBOL_UP : " " LV_SYMBOL_DOWN;
    lv_label_set_text(c.fold, fold.c_str());
    // The fold line follows the body when it is open, so closing it is
    // where the reading ended.
    lv_obj_move_to_index(c.fold, -1);
}

void HistoryPage::load_more() {
    loading_ = false;
    if (first_ == 0 || cards_.empty()) return;
    lv_obj_t* anchor = cards_.front().card;
    lv_obj_update_layout(list_);
    const int32_t before = lv_obj_get_y(anchor) - lv_obj_get_scroll_y(list_);
    const size_t from = first_ > kHistoryBatch ? first_ - kHistoryBatch : 0;
    add_cards(from, first_);
    first_ = from;
    detail::bubble_tree(list_);
    lv_obj_update_layout(list_);
    // The turn that was at the top stays where it was on the screen.
    lv_obj_scroll_to_y(list_, lv_obj_get_y(anchor) - before, LV_ANIM_OFF);
}

void HistoryPage::list_cb(lv_event_t* e) {
    auto* self = static_cast<HistoryPage*>(lv_event_get_user_data(e));
    if (lv_event_get_target(e) != self->list_) return;
    if (self->first_ == 0 || self->loading_ || lv_obj_get_scroll_top(self->list_) > 0) return;
    // Not inside the scroll event that found the top: the cards it adds
    // move the list under it.
    self->loading_ = true;
    if (!self->deps_.ui.post([self, alive = self->alive_] {
            if (*alive) self->load_more();
        })) {
        self->loading_ = false;
    }
}

void HistoryPage::card_cb(lv_event_t* e) {
    auto* self = static_cast<HistoryPage*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_current_target_obj(e);
    for (size_t i = 0; i < self->cards_.size(); ++i) {
        if (self->cards_[i].card == target) {
            self->toggle(i);
            return;
        }
    }
}

void HistoryPage::new_cb(lv_event_t* e) {
    auto* self = static_cast<HistoryPage*>(lv_event_get_user_data(e));
    self->ask_new();
}

void HistoryPage::ask_new() {
    agent::AgentService* agent = deps_.agent;
    if (agent == nullptr) return;
    const agent::ConversationView v = agent->view();
    if (v.running_turn || v.queued || v.state != agent::AgentState::Idle) {
        show_busy();
        return;
    }
    ui::UiRuntime* ui = &deps_.ui;
    std::shared_ptr<bool> alive = alive_;
    lv_obj_t* box = deps_.layer.open_confirm(
        ui::StrId::HistoryNew, ui::tr(ui::StrId::HistoryNewConfirm),
        [this, agent, ui, alive](bool yes) {
            if (!yes || !*alive) return;
            Result<void> r = agent->new_conversation_async([this, ui, alive](Result<void> done) {
                // On wisp.agent.
                if (!ui->post([this, alive, done] {
                        if (*alive) archived(done);
                    })) {
                    WLOG_W(kTag, "history not refreshed after the new conversation");
                }
            });
            if (!r) archived(r);
        },
        ui::ButtonRole::Primary, ui::StrId::Yes, ui::StrId::No);
    // Another system dialog is up (one at a time): this tap asks nothing.
    if (box == nullptr) WLOG_W(kTag, "new conversation not asked: a dialog is open");
}

void HistoryPage::archived(const Result<void>& r) {
    if (!r) {
        WLOG_W(kTag, "new conversation not started: %s", r.error().message.c_str());
        if (r.error().code == ErrorCode::Busy) show_busy();
        return;
    }
    WLOG_I(kTag, "new conversation");
    rebuild();
}

void HistoryPage::show_busy() {
    if (busy_ == nullptr) return;
    lv_obj_remove_flag(busy_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(list_);
    lv_obj_scroll_to_view(busy_, LV_ANIM_OFF);
    if (busy_timer_ != nullptr) lv_timer_delete(busy_timer_);
    busy_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<HistoryPage*>(lv_timer_get_user_data(t));
            self->busy_timer_ = nullptr;
            if (self->busy_ != nullptr) lv_obj_add_flag(self->busy_, LV_OBJ_FLAG_HIDDEN);
            lv_timer_delete(t);
        },
        kBusyShowMs, this);
}

}  // namespace wisp::shell
