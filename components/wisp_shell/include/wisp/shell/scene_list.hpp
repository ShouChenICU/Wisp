#pragma once

// The scene list page (MVP spec 6.4): a grid of cards, one per installed
// scene, in SceneRecents::order() (most recently opened first).
//
// Columns: as many kCardMinPx cards as fit across the safe area, sharing
// its width; each card is square. A card shows the scene's icon or, without
// one, the first character of its name, and the name below. The icon comes
// already decoded and scaled to icon_side() (decode_scene_icon(), run by
// the shell on the storage worker), so building a card decodes nothing and
// drawing it is a plain copy, never a per-frame transform. A scene running
// in the background carries LV_SYMBOL_PLAY; one that cannot run now (its
// files are on an SD card that is out) is greyed out and takes no tap or
// long press. A tap opens the scene; a long press opens a small card over
// the page with "delete" (SceneDeleteButton) and, when the scene has data,
// "delete its data too" (SceneDeleteData). Delete hands both to
// SceneListDeps::delete_scene, which asks with the system dialog. With no
// scenes the page says how to get one (SceneListEmpty).
//
// The scenes come from the shell (ShellDeps::list_scenes on the storage
// worker), handed in with set_scenes() on the UI thread. Until the first
// set_scenes() the page shows it is loading: dim placeholder cards, only
// as many rows as lie wholly inside the safe area, cheap to draw since
// they slide along with a page drag. A listing the same as the one shown
// builds nothing. Icons may come after their cards (set_icon()): a card
// shows the first character until its icon is there.
//
// Threading: everything on the UI thread, the destructor included.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"

namespace wisp::shell {

// A decoded icon: ARGB8888 in PSRAM, `image` pointing into `pixels`.
struct SceneIcon {
    lv_image_dsc_t image{};
    os::LargeBuffer pixels;
};

struct ShellScene {
    std::string id, name;
    // The icon file's bytes (at most kIconMaxBytes), or null. What
    // ShellDeps::list_scenes hands over; the shell turns it into `icon`.
    std::shared_ptr<const os::LargeBuffer> icon_png;
    // The icon as the card draws it, or null (the first character instead).
    std::shared_ptr<const SceneIcon> icon;
    bool background = false;  // running in the background
    bool available = true;
    bool has_data = false;  // a data or files directory to delete with it
};

// The largest icon file the list reads.
inline constexpr size_t kIconMaxBytes = 32 * 1024;
// An icon file's largest side; larger ones are not shown.
inline constexpr uint32_t kIconMaxSide = 256;

// Decodes the PNG `png` and scales it to fit a `side` x `side` square,
// keeping its aspect (averaging when it shrinks). Null for anything that
// is not a PNG of at most kIconMaxSide a side, or without the memory.
// Changes no LVGL state, so it runs on any thread once lv_init() has run
// (the shell runs it on the storage worker); its memory comes from
// Mem::Large.
std::shared_ptr<const SceneIcon> decode_scene_icon(const os::LargeBuffer& png, int32_t side);

struct SceneListDeps {
    hal::ScreenShape shape;
    // Opens the scene in front. Empty: a tap does nothing.
    std::function<void(const std::string&)> open_scene;
    // Asks the user and deletes (SceneWiring::request_delete). Empty: the
    // long press does nothing.
    std::function<Result<void>(const std::string&, bool with_data)> delete_scene;
};

class SceneList {
public:
    // The least side of a card.
    static constexpr int32_t kCardMinPx = 96;
    // The side of the square an icon is drawn in, on a screen of `shape`.
    static int32_t icon_side(const hal::ScreenShape& shape);

    explicit SceneList(SceneListDeps deps);
    ~SceneList();

    SceneList(const SceneList&) = delete;
    SceneList& operator=(const SceneList&) = delete;

    // Builds into `list` (a page's list, page_frame.hpp), showing the
    // loading placeholders until set_scenes().
    void build(lv_obj_t* list);
    // Replaces the cards, keeping the scroll position as far as it goes.
    // The long-press card stays up while its scene is still listed and
    // can run. Nothing changes when `scenes` is what is already shown.
    void set_scenes(std::vector<ShellScene> scenes);
    // Shows `icon` on scene `id`'s card in place of its first character
    // (null: the character again). No card for `id`: nothing.
    void set_icon(const std::string& id, std::shared_ptr<const SceneIcon> icon);

    // The loading placeholders still up (0 once set_scenes() came).
    size_t placeholders() const { return placeholders_; }
    // How often the cards were built, and icons swapped into built cards.
    uint32_t card_builds() const { return card_builds_; }
    uint32_t icon_swaps() const { return icon_swaps_; }

    // For tests and the simulator's checks.
    size_t cards() const { return cards_.size(); }
    lv_obj_t* card(size_t i) const { return i < cards_.size() ? cards_[i].card : nullptr; }
    const ShellScene& scene(size_t i) const { return scenes_[i]; }
    // The background mark, the name label and the icon (null when absent).
    lv_obj_t* mark(size_t i) const { return i < cards_.size() ? cards_[i].mark : nullptr; }
    lv_obj_t* name_label(size_t i) const { return i < cards_.size() ? cards_[i].name : nullptr; }
    lv_obj_t* icon(size_t i) const { return i < cards_.size() ? cards_[i].icon : nullptr; }
    lv_obj_t* empty_label() const { return empty_; }
    lv_obj_t* placeholder(size_t i) const {
        return i < placeholders_ ? lv_obj_get_child(grid_, static_cast<int32_t>(i)) : nullptr;
    }
    int columns() const { return columns_; }
    // The long-press card while it is up, and its parts.
    lv_obj_t* popup() const { return popup_; }
    lv_obj_t* popup_delete() const { return pop_delete_; }
    lv_obj_t* popup_data() const { return pop_data_; }
    void close_popup();

    // What a tap and a long press on card `i` do.
    void tap(size_t i);
    void long_press(size_t i);

private:
    struct Card {
        lv_obj_t* card = nullptr;
        lv_obj_t* icon = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* mark = nullptr;
        lv_obj_t* letter = nullptr;  // without an icon
    };
    void show_loading();
    void build_cards();
    void put_icon(Card& c, const ShellScene& s);
    size_t index_of(lv_obj_t* card) const;
    static void card_cb(lv_event_t* e);
    static void popup_cb(lv_event_t* e);

    SceneListDeps deps_;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* grid_ = nullptr;
    lv_obj_t* empty_ = nullptr;
    int columns_ = 1;
    int32_t card_px_ = kCardMinPx;
    size_t placeholders_ = 0;
    bool listed_ = false;  // set_scenes() came
    uint32_t card_builds_ = 0, icon_swaps_ = 0;
    std::vector<ShellScene> scenes_;
    std::vector<Card> cards_;
    lv_obj_t* popup_ = nullptr;
    lv_obj_t* pop_delete_ = nullptr;
    lv_obj_t* pop_data_ = nullptr;
    std::string pop_id_;
};

}  // namespace wisp::shell
