// Chinese (zh) device UI text -- the default language and the fallback
// for any id the other tables lack (spec 8.3).

#include "strings_tables.hpp"

namespace wisp::ui::detail {

const char* strings_zh(StrId id) {
    switch (id) {
        case StrId::BootTime:
            return "启动";
        case StrId::StateOk:
            return "正常";
        case StrId::StateDegraded:
            return "降级";
        case StrId::StateMissing:
            return "缺失";
        case StrId::StateFailed:
            return "失败";
        case StrId::SummaryDevices:
            return "设备 {} 正常";
        case StrId::SummaryTests:
            return "交互 {} 已测";
        case StrId::SectionDevices:
            return "设备";
        case StrId::SectionNetwork:
            return "网络";
        case StrId::SectionSensors:
            return "传感器";
        case StrId::SectionPowerClock:
            return "电源与时钟";
        case StrId::SectionStorage:
            return "存储";
        case StrId::SectionTests:
            return "交互测试";
        case StrId::SectionPerf:
            return "性能";
        case StrId::SelfCheckTitle:
            return "自检";
        case StrId::SectionBoard:
            return "设备信息";
        case StrId::BoardName:
            return "开发板";
        case StrId::BoardTarget:
            return "目标平台";
        case StrId::Firmware:
            return "固件";
        case StrId::LangNameEn:
            return "EN";
        case StrId::LangNameZh:
            return "中文";
        case StrId::LangNameJa:
            return "日本語";
        case StrId::Battery:
            return "电池";
        case StrId::Time:
            return "时间";
        case StrId::TimeSync:
            return "网络校时";
        case StrId::InternalFlash:
            return "内置 Flash";
        case StrId::SdCard:
            return "SD 卡";
        case StrId::Button:
            return "按键";
        case StrId::Touch:
            return "触点";
        case StrId::Accel:
            return "加速度";
        case StrId::Gyro:
            return "角速度";
        case StrId::TouchTest:
            return "触摸";
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
        case StrId::TestUntested:
            return "未测";
        case StrId::TestPassed:
            return "已通过";
        case StrId::TimeSynced:
            return "已同步 {}";
        case StrId::TimeNotSynced:
            return "未同步";
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
        case StrId::NetLine:
            return "Wi-Fi：{}";
        case StrId::NetNotSetUp:
            return "未配网";
        case StrId::NetOff:
            return "未连接";
        case StrId::NetConnecting:
            return "连接中…";
        case StrId::NetRetrying:
            return "重试中（{} 秒后）";
        case StrId::NetRejected:
            return "{} 的密码被拒绝，请重新配网";
        case StrId::NetRejectedNoName:
            return "密码被拒绝，请重新配网";
        case StrId::NetConnected:
            return "已连接";
        case StrId::NetNoHardware:
            return "无 Wi-Fi 硬件";
        case StrId::StartSetup:
            return "开始配网";
        case StrId::StopSetup:
            return "停止配网";
        case StrId::SetupTitle:
            return "配网";
        case StrId::Hotspot:
            return "热点：{}";
        case StrId::Password:
            return "密码：{}";
        case StrId::OpenAddress:
            return "连上后打开 {}";
        case StrId::PairTitle:
            return "配对请求";
        case StrId::PairRemaining:
            return "剩余 {} 秒";
        case StrId::Reject:
            return "拒绝";
        case StrId::ResetTitle:
            return "恢复出厂设置";
        case StrId::ResetPrompt:
            return "将清除 Wi-Fi、配对和模型配置并重启，场景数据保留。";
        case StrId::FirmwareTitle:
            return "固件更新";
        case StrId::FirmwarePrompt:
            return "安装新固件并重启？";
        case StrId::FirmwareDowngrade:
            return "注意：新固件比当前版本旧（降级）。";
        case StrId::Scenes:
            return "场景";
        case StrId::ScenesEmpty:
            return "还没有安装场景";
        case StrId::SceneStateRunning:
            return "运行中";
        case StrId::SceneStateError:
            return "出错";
        case StrId::SceneErrorTitle:
            return "场景出错";
        case StrId::SceneReload:
            return "重载";
        case StrId::SceneBack:
            return "返回";
        case StrId::SceneDeleteTitle:
            return "删除场景";
        case StrId::SceneDeletePrompt:
            return "删除场景“{}”？";
        case StrId::SceneDeleteWithData:
            return "场景数据也会一并删除。";
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
