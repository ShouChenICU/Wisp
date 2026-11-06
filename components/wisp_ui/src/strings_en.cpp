// English (en) device UI text (spec 8.3).

#include "strings_tables.hpp"

namespace wisp::ui::detail {

const char* strings_en(StrId id) {
    switch (id) {
        case StrId::BootTime:
            return "Boot";
        case StrId::StateOk:
            return "OK";
        case StrId::StateDegraded:
            return "Degraded";
        case StrId::StateMissing:
            return "Missing";
        case StrId::StateFailed:
            return "Failed";
        case StrId::SummaryDevices:
            return "Devices {} OK";
        case StrId::SummaryTests:
            return "Tests {} done";
        case StrId::SectionDevices:
            return "Devices";
        case StrId::SectionNetwork:
            return "Network";
        case StrId::SectionSensors:
            return "Sensors";
        case StrId::SectionPowerClock:
            return "Power & clock";
        case StrId::SectionStorage:
            return "Storage";
        case StrId::SectionTests:
            return "Interactive tests";
        case StrId::SectionPerf:
            return "Performance";
        case StrId::SelfCheckTitle:
            return "Self Check";
        case StrId::SectionBoard:
            return "Board";
        case StrId::BoardName:
            return "Board";
        case StrId::BoardTarget:
            return "Target";
        case StrId::Firmware:
            return "Firmware";
        case StrId::LangNameEn:
            return "EN";
        case StrId::LangNameZh:
            return "中文";
        case StrId::LangNameJa:
            return "日本語";
        case StrId::Battery:
            return "Battery";
        case StrId::Time:
            return "Time";
        case StrId::TimeSync:
            return "Time sync";
        case StrId::InternalFlash:
            return "Internal flash";
        case StrId::SdCard:
            return "SD card";
        case StrId::Button:
            return "Button";
        case StrId::Touch:
            return "Touch";
        case StrId::Accel:
            return "Accel";
        case StrId::Gyro:
            return "Gyro";
        case StrId::TouchTest:
            return "Touch";
        case StrId::PerfInternalRam:
            return "Internal RAM";
        case StrId::PerfPsram:
            return "PSRAM";
        case StrId::ReadFailed:
            return "Read error";
        case StrId::LevelUnknown:
            return "Level unknown";
        case StrId::Charging:
            return "Charging";
        case StrId::NotCharging:
            return "Not charging";
        case StrId::Online:
            return "Online";
        case StrId::Offline:
            return "Offline";
        case StrId::PerfFree:
            return "free";
        case StrId::PerfLargestBlock:
            return "largest block";
        case StrId::PerfTotal:
            return "total";
        case StrId::PerfUsed:
            return "used";
        case StrId::TouchTestArea:
            return "Touch test area";
        case StrId::TestUntested:
            return "Not tested";
        case StrId::TestPassed:
            return "Passed";
        case StrId::TimeSynced:
            return "Synced {}";
        case StrId::TimeNotSynced:
            return "Not synced";
        case StrId::LoopbackButton:
            return "Mic test";
        case StrId::LoopReady:
            return "Ready";
        case StrId::LoopRecording:
            return "Recording";
        case StrId::LoopPlaying:
            return "Playing";
        case StrId::LoopDone:
            return "Done";
        case StrId::LoopFailed:
            return "Failed";
        case StrId::PowerOffTitle:
            return "Power off";
        case StrId::PowerOffPrompt:
            return "Turn off the device?";
        case StrId::Yes:
            return "Yes";
        case StrId::No:
            return "No";
        case StrId::NetLine:
            return "Wi-Fi: {}";
        case StrId::NetNotSetUp:
            return "Not set up";
        case StrId::NetOff:
            return "Not connected";
        case StrId::NetConnecting:
            return "Connecting…";
        case StrId::NetRetrying:
            return "Retrying in {} s";
        case StrId::NetRejected:
            return "{} rejected the password. Set up Wi-Fi again";
        case StrId::NetRejectedNoName:
            return "Password rejected. Set up Wi-Fi again";
        case StrId::NetConnected:
            return "Connected";
        case StrId::NetNoHardware:
            return "No Wi-Fi hardware";
        case StrId::StartSetup:
            return "Set up Wi-Fi";
        case StrId::StopSetup:
            return "Stop setup";
        case StrId::SetupTitle:
            return "Wi-Fi setup";
        case StrId::Hotspot:
            return "Hotspot: {}";
        case StrId::Password:
            return "Password: {}";
        case StrId::OpenAddress:
            return "Then open {}";
        case StrId::PairTitle:
            return "Pairing request";
        case StrId::PairRemaining:
            return "{} s left";
        case StrId::Reject:
            return "Reject";
        case StrId::ResetTitle:
            return "Factory reset";
        case StrId::ResetPrompt:
            return "Clears Wi-Fi, pairings and model settings, then restarts. Scene data is kept.";
        case StrId::FirmwareTitle:
            return "Firmware update";
        case StrId::FirmwarePrompt:
            return "Install the new firmware and restart?";
        case StrId::FirmwareDowngrade:
            return "Note: this is older than the running version (a downgrade).";
        case StrId::Scenes:
            return "Scenes";
        case StrId::ScenesEmpty:
            return "No scenes installed";
        case StrId::SceneStateRunning:
            return "Running";
        case StrId::SceneStateError:
            return "Error";
        case StrId::SceneErrorTitle:
            return "Scene error";
        case StrId::SceneReload:
            return "Reload";
        case StrId::SceneBack:
            return "Back";
        case StrId::SceneDeleteTitle:
            return "Delete scene";
        case StrId::SceneDeletePrompt:
            return "Delete the scene \"{}\"?";
        case StrId::SceneDeleteWithData:
            return "Its data is deleted too.";
        case StrId::NoticeSystemTitle:
            return "System";
        case StrId::NoticeEvictedMemory:
            return "{} was closed to free memory";
        case StrId::NoticeEvictedCount:
            return "{} was closed: too many background scenes";
        case StrId::NoticeSceneError:
            return "{} stopped after an error";
        case StrId::NoticeRestoreFailed:
            return "{} could not be restored at startup";
        case StrId::NoticeDeleteFailed:
            return "{} could not be deleted; try again";
        case StrId::NoticeWakeLock:
            return "{} has kept the device awake for 30 minutes";
        case StrId::NoticeBatteryLow:
            return "Battery low: {}%";
        case StrId::NoticeRelease:
            return "Release";
        case StrId::HolderAdmin:
            return "Admin page";
        case StrId::HolderUpload:
            return "Upload";
        case StrId::HolderOta:
            return "Firmware update";
        case StrId::HolderTimeSync:
            return "Time sync";
        case StrId::HolderNetWindow:
            return "Background network";
        case StrId::HolderHotspot:
            return "Setup hotspot";
        case StrId::HolderTone:
            return "Alert sound";
        case StrId::HolderDownload:
            return "File download";
        case StrId::PanelBackground:
            return "In background";
        case StrId::PanelNotifications:
            return "Notifications";
        case StrId::PanelClearAll:
            return "Clear all";
        case StrId::PanelEmpty:
            return "No notifications";
        case StrId::PanelJustNow:
            return "just now";
        case StrId::PanelMinutes:
            return "{} min";
        case StrId::PanelHours:
            return "{} h";
        case StrId::AgentStepLimit:
            return "Too many steps; stopped";
        case StrId::AgentTimeLimit:
            return "Over 10 minutes; stopped";
        case StrId::AgentStopped:
            return "Stopped";
        case StrId::AgentInterrupted:
            return "Interrupted";
        case StrId::AgentErrNoModel:
            return "No model is set up yet. Add and enable one on the Model page of the admin "
                   "console";
        case StrId::AgentErrNetwork:
            return "No network connection. Check Wi-Fi";
        case StrId::AgentErrNetworkLost:
            return "Network connection lost";
        case StrId::AgentErrUnauthorized:
            return "The model refused the API Key. Check its settings on the Model page of the "
                   "admin console";
        case StrId::AgentErrServer:
            return "The model service returned an error. Try again later";
        case StrId::AgentRolledOver:
            return "The conversation got long, so it was archived and a new one started";
        case StrId::AgentContextDropped:
            return "The conversation got long and its older part could not be summarized, so it "
                   "was left out of the context (the record is kept)";
        case StrId::NoticeAgentDone:
            return "Done: {}";
        case StrId::NoticeSettingsNotSaved:
            return "Settings could not be saved";
        case StrId::AgentNoteInstalled:
            return "Installed \"{}\"";
        case StrId::AgentVersionSuffix:
            return " {}";
        case StrId::AgentNoteDiscarded:
            return "Discarded the draft of \"{}\"";
        case StrId::AgentNoteRolledBack:
            return "Rolled \"{}\" back to its previous version";
        case StrId::AgentConfirmHosts:
            return "\"{}\" will be able to reach ";
        case StrId::AgentListSep:
            return ", ";
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
