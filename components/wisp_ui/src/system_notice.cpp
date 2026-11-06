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
    static constexpr std::pair<const char*, StrId> kSystem[] = {
        {"system.admin", StrId::HolderAdmin},
        {"system.upload", StrId::HolderUpload},
        {"system.ota", StrId::HolderOta},
        {"system.ntp", StrId::HolderTimeSync},
        {"system.netwindow", StrId::HolderNetWindow},
        {"system.hotspot", StrId::HolderHotspot},
        {"system.tone", StrId::HolderTone},
        {"system.download", StrId::HolderDownload},
    };
    for (const auto& [h, id] : kSystem) {
        if (holder == h) return tr(id);
    }
    constexpr std::string_view kScene = "scene.";
    if (holder.substr(0, kScene.size()) == kScene) {
        return std::string(scene_name.empty() ? holder.substr(kScene.size()) : scene_name);
    }
    return std::string(holder);
}

}  // namespace wisp::ui
