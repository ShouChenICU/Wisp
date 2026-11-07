#pragma once

// The conversation history page (MVP spec 6.4).
//
// group_turns() folds the agent's conversation into turns: the user's
// message (and where it came from), the reply's <say> text, and its body
// (voice::split_say() over every assistant text of the turn, in order,
// then the turn's notes, each on a line of its own). Tool calls and
// results, summaries and markers are left out; so is a turn with nothing
// to show.
//
// HistoryPage lists them oldest at the top, newest at the bottom, and opens
// scrolled to the bottom. It builds the newest kHistoryBatch turns; once
// the list is scrolled to its top it adds kHistoryBatch earlier ones
// above, keeping in view the turn that was at the top. Each turn is a
// card: the user's message (LV_SYMBOL_AUDIO before a spoken one, in the
// secondary colour), the say text, and, when there is a body, a line
// saying so; tapping the card unfolds the body (light_markdown()) and
// tapping again folds it. Below the turns sits "new conversation"
// (HistoryNew): it asks first (HistoryNewConfirm, through the system
// layer's dialog) and then archives the conversation
// (AgentService::new_conversation_async()), after which the page lists
// the new, empty one. While a turn runs or a message waits it shows
// HistoryBusy instead, for kBusyShowMs.
//
// Memory: the grouped turns are in PSRAM (os::LargeString); the cards are
// LVGL objects, in LVGL's heap.
//
// Threading: everything on the UI thread, the destructor included. The
// archive's answer arrives on wisp.agent and is posted; a post that finds
// the page gone does nothing. The conversation is read with
// AgentService::view(), a snapshot in memory: nothing here touches flash.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "lvgl.h"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/os/large.hpp"

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
}  // namespace wisp::ui

namespace wisp::shell {

struct HistoryTurn {
    uint32_t turn = 0;
    agent::Source source = agent::Source::Web;
    os::LargeString heard, say, body;
};

// Oldest first.
os::LargeVector<HistoryTurn> group_turns(std::span<const agent::Message> msgs);

struct HistoryDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    agent::AgentService* agent = nullptr;  // null: an empty history
};

class HistoryPage {
public:
    // How long HistoryBusy stays up.
    static constexpr uint32_t kBusyShowMs = 4000;

    explicit HistoryPage(HistoryDeps deps);
    // Deletes the busy timer; the list itself goes with its screen.
    ~HistoryPage();

    HistoryPage(const HistoryPage&) = delete;
    HistoryPage& operator=(const HistoryPage&) = delete;

    // Fills `list` (a page's list, page_frame.hpp) and scrolls it to the
    // bottom.
    void build(lv_obj_t* list);

    // Scrolled all the way down (where the page opens): an up swipe then
    // goes home (spec 6.3).
    bool at_bottom() const;

    // For tests and the simulator's checks.
    size_t total() const { return turns_.size(); }
    size_t built() const { return turns_.size() - first_; }
    // The card of turns_[first_ + i], the oldest built first.
    lv_obj_t* card(size_t i) const;
    // The card's parts: the user's message, the say text, the fold line
    // (null without a body) and the body (null until first unfolded).
    lv_obj_t* heard_label(size_t i) const;
    lv_obj_t* say_label(size_t i) const;
    lv_obj_t* fold_label(size_t i) const;
    lv_obj_t* body_box(size_t i) const;
    bool expanded(size_t i) const;
    lv_obj_t* new_button() const { return new_btn_; }
    lv_obj_t* busy_label() const { return busy_; }
    lv_obj_t* list() const { return list_; }

    // What tapping the card, and "new conversation", do.
    void toggle(size_t i);
    void ask_new();

private:
    struct Card {
        lv_obj_t* card = nullptr;
        lv_obj_t* heard = nullptr;
        lv_obj_t* say = nullptr;
        lv_obj_t* fold = nullptr;
        lv_obj_t* body = nullptr;
        bool open = false;
    };

    void rebuild();
    // Builds the cards for turns_[from, to) at the top of the list.
    void add_cards(size_t from, size_t to);
    void load_more();
    void show_busy();
    void archived(const Result<void>& r);
    static void card_cb(lv_event_t* e);
    static void list_cb(lv_event_t* e);
    static void new_cb(lv_event_t* e);

    HistoryDeps deps_;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* empty_ = nullptr;
    lv_obj_t* busy_ = nullptr;
    lv_obj_t* new_btn_ = nullptr;
    lv_timer_t* busy_timer_ = nullptr;
    os::LargeVector<HistoryTurn> turns_;
    // The cards of turns_[first_..], in that order.
    os::LargeVector<Card> cards_;
    size_t first_ = 0;
    bool loading_ = false;
    // Cleared by the destructor: posts queued before it do nothing.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace wisp::shell
