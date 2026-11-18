#include "wisp/ui/system_notice.hpp"

#include <utility>

#include "wisp/core/notification_service.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::ui {

NoticeText system_notice_text(std::string_view code, std::string_view arg) {
    namespace notice = core::notice;
    static constexpr std::pair<const char*, StrId> kBodies[] = {
        {notice::kSceneEvictedMemory, StrId::NoticeEvictedMemory},
        {notice::kSceneEvictedCount, StrId::NoticeEvictedCount},
        {notice::kSceneError, StrId::NoticeSceneError},
        {notice::kRestoreFailed, StrId::NoticeRestoreFailed},
        {notice::kSceneDeleteFailed, StrId::NoticeDeleteFailed},
        {notice::kWakeLockLong, StrId::NoticeWakeLock},
        {notice::kBatteryLow, StrId::NoticeBatteryLow},
        {notice::kAgentDone, StrId::NoticeAgentDone},
        {notice::kSettingsNotSaved, StrId::NoticeSettingsNotSaved},
    };
    NoticeText t{tr(StrId::NoticeSystemTitle), std::string(code)};
    for (const auto& [c, id] : kBodies) {
        if (code == c) {
            t.body = tr_fill(id, arg);
            break;
        }
    }
    return t;
}

std::string holder_label(std::string_view holder, std::string_view scene_name) {
    // Every system holder that takes a wake lock (grep core::WakeLock( and
    // the kHolder constants under components/).
    static constexpr std::pair<const char*, StrId> kSystem[] = {
        {"system.upload", StrId::HolderUpload},
        {"system.ota", StrId::HolderOta},
        {"system.ntp", StrId::HolderTimeSync},
        {"system.netwindow", StrId::HolderNetWindow},
        {"system.hotspot", StrId::HolderHotspot},
        {"system.tone", StrId::HolderTone},
        {"system.download", StrId::HolderDownload},
        {"system.powered_wifi", StrId::HolderPoweredWifi},
        {"system.agent", StrId::HolderAgent},
        {"system.voice", StrId::HolderVoice},
    };
    for (const auto& [h, id] : kSystem) {
        if (holder == h) return tr(id);
    }
    constexpr std::string_view kScene = "scene.";
    if (holder.substr(0, kScene.size()) == kScene) {
        return std::string(scene_name.empty() ? holder.substr(kScene.size()) : scene_name);
    }
    // A holder id is not text for the user: one without a label here
    // reads as a generic phrase.
    return tr(StrId::HolderSystem);
}

}  // namespace wisp::ui
