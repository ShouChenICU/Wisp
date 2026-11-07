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
        case StrId::PerfInternalRam:
            return "内部 RAM";
        case StrId::PerfPsram:
            return "PSRAM";
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
        case StrId::PerfFree:
            return "空闲";
        case StrId::PerfLargestBlock:
            return "最大块";
        case StrId::PerfTotal:
            return "共";
        case StrId::PerfUsed:
            return "已用";
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
        case StrId::SetupDoneTitle:
            return "已连接 {}";
        case StrId::SetupDoneAddress:
            return "管理页面：{}";
        case StrId::SetupDoneOk:
            return "确定（{}）";
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
        case StrId::NoticeSystemTitle:
            return "系统";
        case StrId::NoticeEvictedMemory:
            return "{}已因内存不足关闭";
        case StrId::NoticeEvictedCount:
            return "{}已关闭：后台场景已满";
        case StrId::NoticeSceneError:
            return "{}出错已停止";
        case StrId::NoticeRestoreFailed:
            return "{}开机恢复失败";
        case StrId::NoticeDeleteFailed:
            return "{}未能删除，请重试";
        case StrId::NoticeWakeLock:
            return "{}已让设备保持唤醒 30 分钟";
        case StrId::NoticeBatteryLow:
            return "电量低：{}%";
        case StrId::NoticeRelease:
            return "释放";
        case StrId::HolderAdmin:
            return "管理后台";
        case StrId::HolderUpload:
            return "上传";
        case StrId::HolderOta:
            return "固件更新";
        case StrId::HolderTimeSync:
            return "校时";
        case StrId::HolderNetWindow:
            return "后台联网";
        case StrId::HolderHotspot:
            return "配网热点";
        case StrId::HolderTone:
            return "提示音";
        case StrId::HolderDownload:
            return "文件下载";
        case StrId::PanelBackground:
            return "后台运行";
        case StrId::PanelNotifications:
            return "通知";
        case StrId::PanelClearAll:
            return "全部清除";
        case StrId::PanelEmpty:
            return "没有通知";
        case StrId::PanelJustNow:
            return "刚刚";
        case StrId::PanelMinutes:
            return "{} 分钟";
        case StrId::PanelHours:
            return "{} 小时";
        case StrId::AgentStepLimit:
            return "步骤太多，已停止";
        case StrId::AgentTimeLimit:
            return "超过 10 分钟，已停止";
        case StrId::AgentStopped:
            return "已中止";
        case StrId::AgentInterrupted:
            return "已中断";
        case StrId::AgentErrNoModel:
            return "还没有可用的模型，请在管理后台的“模型”页添加并启用";
        case StrId::AgentErrNetwork:
            return "网络不可用，请检查 Wi-Fi";
        case StrId::AgentErrNetworkLost:
            return "网络中断";
        case StrId::AgentErrUnauthorized:
            return "模型拒绝了 API Key，请在管理后台的“模型”页检查配置";
        case StrId::AgentErrServer:
            return "模型服务出错，请稍后再试";
        case StrId::AgentRolledOver:
            return "对话太长，已存档并开始新对话";
        case StrId::AgentContextDropped:
            return "对话太长，较早的内容没能总结，已从上下文中省略（记录仍保留）";
        case StrId::NoticeAgentDone:
            return "完成：{}";
        case StrId::NoticeSettingsNotSaved:
            return "设置未能保存";
        case StrId::AgentNoteInstalled:
            return "已安装「{}」";
        case StrId::AgentVersionSuffix:
            return "{}";
        case StrId::AgentNoteDiscarded:
            return "已丢弃「{}」的草稿";
        case StrId::AgentNoteRolledBack:
            return "已把「{}」恢复到上一个版本";
        case StrId::AgentConfirmHosts:
            return "「{}」将可以访问 ";
        case StrId::AgentListSep:
            return "、";
        case StrId::AgentNoteOpened:
            return "打开了「{}」";
        case StrId::AgentNoteClosed:
            return "关闭了「{}」";
        case StrId::AgentNoteToBackground:
            return "「{}」转入后台";
        case StrId::AgentNoteExited:
            return "退出了「{}」";
        case StrId::AgentNoteDataWritten:
            return "改写了「{}」的数据 {}";
        case StrId::AgentNoteEval:
            return "在「{}」中执行了 Lua";
        case StrId::AgentNoteDeleted:
            return "删除了「{}」";
        case StrId::AgentNoteFileRemoved:
            return "删除了「{}」的文件 {}";
        case StrId::AgentConfirmDelete:
            return "Agent 要删除场景「{}」";
        case StrId::AgentConfirmWithData:
            return "及其数据";
        case StrId::AgentConfirmFileRemove:
            return "Agent 要删除「{}」的文件 {}";
        case StrId::AgentNoteSetting:
            return "{} {} → {}";
        case StrId::AgentSetBrightness:
            return "亮度";
        case StrId::AgentSetVolume:
            return "音量";
        case StrId::AgentSetNotifyVol:
            return "提示音量";
        case StrId::AgentSetScreenOff:
            return "熄屏时间";
        case StrId::AgentSetLanguage:
            return "语言";
        case StrId::AgentConfirmTitle:
            return "Agent 请求确认";
        case StrId::AgentAllow:
            return "允许";
        case StrId::AgentDeny:
            return "拒绝";
        case StrId::AgentConfirmClear:
            return "清空全部 Agent 对话？";
        case StrId::AgentNoteMemoryAdded:
            return "已记住一条记忆";
        case StrId::AgentNoteMemoryUpdated:
            return "已修改记忆 #{}";
        case StrId::AgentNoteMemoryRemoved:
            return "已删除记忆 #{}";
        case StrId::AgentNoteTodoAdded:
            return "已添加待办「{}」";
        case StrId::AgentNoteTodoUpdated:
            return "已修改待办「{}」";
        case StrId::AgentNoteTodoDone:
            return "已完成「{}」";
        case StrId::AgentNoteTodoRemoved:
            return "已删除待办「{}」";
        case StrId::NoteActionTodoDone:
            return "完成";
        case StrId::NoteActionTodoSnooze:
            return "10 分钟后";
        case StrId::ReminderMissed:
            return "错过 {} 个提醒";
        case StrId::HomeNoWifi:
            return "还没连接 Wi-Fi，右划进入设置 → Wi-Fi";
        case StrId::HomeNoLlm:
            return "还没配置模型，请在管理后台的模型页设置";
        case StrId::HomeNotHeard:
            return "没听清";
        case StrId::VoiceErrNoAsr:
            return "还没配置语音识别，请在管理后台的模型页设置";
        case StrId::VoiceErrNoTts:
            return "还没配置语音合成，请在管理后台的模型页设置";
        case StrId::VoiceErrOffline:
            return "网络未连接，暂时不能对话";
        case StrId::VoiceErrAsr:
            return "语音识别失败，请再说一次";
        case StrId::VoiceErrTts:
            return "朗读失败，文字照常显示";
        case StrId::VoiceErrMic:
            return "麦克风不可用";
        case StrId::PageSettings:
            return "设置";
        case StrId::PageScenes:
            return "场景";
        case StrId::PageHistory:
            return "对话记录";
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
