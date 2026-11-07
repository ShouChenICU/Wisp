#pragma once

// The scene list page (MVP spec 6.4): a grid of cards, one per installed
// scene, in SceneRecents::order() (most recently opened first).
//
// Columns: as many kCardMinPx cards as fit across the safe area, sharing
// its width; each card is square. A card shows the scene's icon (the
// manifest's PNG, decoded once into PSRAM and drawn scaled to fit) or, without
// one, the first character of its name, and the name below. A scene running
// in the background carries LV_SYMBOL_PLAY; one that cannot run now (its
// files are on an SD card that is out) is greyed out and takes no tap or
// long press. A tap opens the scene; a long press opens a small card over
// the page with "delete" (SceneDeleteButton) and, when the scene has data,
// "delete its data too" (SceneDeleteData). Delete hands both to
// SceneListDeps::delete_scene, which asks with the system dialog. With no
// scenes the page says how to get one (SceneListEmpty).
//
// The scenes come from the shell (ShellDeps::list_scenes on the storage
// worker), handed in with set_scenes() on the UI thread.
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

struct ShellScene {
    std::string id, name;
    // The icon file's bytes (at most kIconMaxBytes), or null.
    std::shared_ptr<const os::LargeBuffer> icon_png;
    bool background = false;  // running in the background
    bool available = true;
    bool has_data = false;  // a data or files directory to delete with it
};

// The largest icon file the list reads.
inline constexpr size_t kIconMaxBytes = 32 * 1024;

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
    // An icon's longest side as decoded; larger ones are not shown.
    static constexpr uint32_t kIconMaxSide = 256;

    explicit SceneList(SceneListDeps deps);
    ~SceneList();

    SceneList(const SceneList&) = delete;
    SceneList& operator=(const SceneList&) = delete;

    // Builds into `list` (a page's list, page_frame.hpp); empty until
    // set_scenes().
    void build(lv_obj_t* list);
    // Replaces the cards, keeping the scroll position as far as it goes.
    void set_scenes(std::vector<ShellScene> scenes);

    // For tests and the simulator's checks.
    size_t cards() const { return cards_.size(); }
    lv_obj_t* card(size_t i) const { return i < cards_.size() ? cards_[i].card : nullptr; }
    const ShellScene& scene(size_t i) const { return scenes_[i]; }
    // The background mark, the name label and the icon (null when absent).
    lv_obj_t* mark(size_t i) const { return i < cards_.size() ? cards_[i].mark : nullptr; }
    lv_obj_t* name_label(size_t i) const { return i < cards_.size() ? cards_[i].name : nullptr; }
    lv_obj_t* icon(size_t i) const { return i < cards_.size() ? cards_[i].icon : nullptr; }
    lv_obj_t* empty_label() const { return empty_; }
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
    };
    // A decoded icon; `image` points into `pixels`.
    struct Icon {
        lv_image_dsc_t image{};
        os::LargeBuffer pixels;
    };

    void build_cards();
    // Decodes `png` into an Icon, or null.
    std::unique_ptr<Icon> decode(const os::LargeBuffer& png) const;
    size_t index_of(lv_obj_t* card) const;
    static void card_cb(lv_event_t* e);
    static void popup_cb(lv_event_t* e);

    SceneListDeps deps_;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* grid_ = nullptr;
    lv_obj_t* empty_ = nullptr;
    int columns_ = 1;
    int32_t card_px_ = kCardMinPx;
    std::vector<ShellScene> scenes_;
    std::vector<Card> cards_;
    std::vector<std::unique_ptr<Icon>> icons_;
    lv_obj_t* popup_ = nullptr;
    lv_obj_t* pop_delete_ = nullptr;
    lv_obj_t* pop_data_ = nullptr;
    std::string pop_id_;
};

}  // namespace wisp::shell
