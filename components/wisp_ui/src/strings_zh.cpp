// Chinese (zh) device UI text -- the default language and the fallback
// for any id the other tables lack (spec 8.3).

#include "strings_tables.hpp"

namespace wisp::ui::detail {

const char* strings_zh(StrId id) {
    switch (id) {
        case StrId::BootTime:
            return "启动";
        case StrId::DeviceList:
            return "设备列表";
        case StrId::StateOk:
            return "正常";
        case StrId::StateDegraded:
            return "降级";
        case StrId::StateMissing:
            return "缺失";
        case StrId::StateFailed:
            return "失败";
        case StrId::Battery:
            return "电池";
        case StrId::Time:
            return "时间";
        case StrId::InternalFlash:
            return "内置 Flash";
        case StrId::SdCard:
            return "SD 卡";
        case StrId::Button:
            return "按键";
        case StrId::Touch:
            return "触点";
        case StrId::ReadFailed:
            return "读取失败";
        case StrId::LevelUnknown:
            return "电量未知";
        case StrId::Charging:
            return "充电中";
        case StrId::NotCharging:
            return "未充电";
        case StrId::Online:
            return "在线";
        case StrId::Offline:
            return "离线";
        case StrId::TouchTestArea:
            return "触摸测试区";
        case StrId::LoopbackButton:
            return "录音回放";
        case StrId::LoopReady:
            return "就绪";
        case StrId::LoopRecording:
            return "录音中";
        case StrId::LoopPlaying:
            return "播放中";
        case StrId::LoopDone:
            return "完成";
        case StrId::LoopFailed:
            return "失败";
        case StrId::PowerOffTitle:
            return "关机确认";
        case StrId::PowerOffPrompt:
            return "是否关机？";
        case StrId::Yes:
            return "是";
        case StrId::No:
            return "否";
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
