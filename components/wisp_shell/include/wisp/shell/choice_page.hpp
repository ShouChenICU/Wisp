#pragma once

// A single-choice list (mvp spec 6.4): what a settings row whose value is
// one of a few opens -- the screen-off time, the language, the time zone.
// The shell shows it as a page of its own over the settings page (the
// same page frame, its title the row's name), and a drag to the right
// takes it back to the settings page (wisp/shell/shell.hpp).
//
// One row per choice, the current one marked (a check and the accent
// colour). A tap marks the row and calls ChoiceSpec::on_pick with its
// index; the shell then closes the page. Built scrolled so that the
// current choice is in the middle of the screen (or as near as it gets).
//
// UI thread only.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/ui/strings.hpp"

namespace wisp::shell {

struct ChoiceSpec {
    ui::StrId title = ui::StrId::PageSettings;
    std::vector<std::string> labels;
    // The marked row; -1: none.
    int current = -1;
    // Called inside the tap's event, which must not delete the page.
    std::function<void(size_t)> on_pick;
};

class ChoicePage {
public:
    explicit ChoicePage(ChoiceSpec spec);

    ChoicePage(const ChoicePage&) = delete;
    ChoicePage& operator=(const ChoicePage&) = delete;

    // Adds the rows to the page's list (after its title).
    void build(lv_obj_t* list);

    lv_obj_t* list() const { return list_; }
    size_t count() const { return rows_.size(); }
    lv_obj_t* row(size_t i) const { return i < rows_.size() ? rows_[i] : nullptr; }
    int current() const { return spec_.current; }

private:
    void mark(int i);
    static void event_cb(lv_event_t* e);

    ChoiceSpec spec_;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* card_ = nullptr;
    std::vector<lv_obj_t*> rows_;
    std::vector<lv_obj_t*> checks_;
};

}  // namespace wisp::shell
