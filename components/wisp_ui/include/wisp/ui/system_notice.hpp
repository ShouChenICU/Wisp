#pragma once

// The words of system notifications (scene-device spec 5.2): a system
// notice carries a code and an argument (core::notice), and the UI turns
// them into text in the device language when it shows them, so a notice
// stored before a language change reads in the new language.
//
// Pure functions over ui::tr(); any thread.

#include <string>
#include <string_view>

namespace wisp::ui {

struct NoticeText {
    std::string title;
    std::string body;
};

// The title and body of system notice `code` in the current ui::lang().
// `arg` goes into the body as it is: a scene notice's argument is already
// the scene's name, and the caller passes a wake lock holder through
// holder_label() first. An unknown code, from a newer firmware's stored list,
// becomes the body itself.
NoticeText system_notice_text(std::string_view code, std::string_view arg);

// What a wake lock holder is called on screen: a "system.<module>" holder
// by its HolderXxx string, a "scene.<id>" holder by `scene_name` (its id
// when the name is empty), anything else as it is.
std::string holder_label(std::string_view holder, std::string_view scene_name);

}  // namespace wisp::ui
