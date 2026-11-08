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
        case StrId::SetupDoneTitle:
            return "Connected to {}";
        case StrId::SetupDoneAddress:
            return "Admin page: {}";
        case StrId::SetupDoneOk:
            return "OK ({})";
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
            return "Over 15 minutes; stopped";
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
        case StrId::AgentNoteOpened:
            return "Opened \"{}\"";
        case StrId::AgentNoteClosed:
            return "Closed \"{}\"";
        case StrId::AgentNoteToBackground:
            return "Moved \"{}\" to the background";
        case StrId::AgentNoteExited:
            return "Exited \"{}\"";
        case StrId::AgentNoteDataWritten:
            return "Changed the data of \"{}\": {}";
        case StrId::AgentNoteEval:
            return "Ran Lua in \"{}\"";
        case StrId::AgentNoteDeleted:
            return "Deleted \"{}\"";
        case StrId::AgentNoteFileRemoved:
            return "Deleted a file of \"{}\": {}";
        case StrId::AgentConfirmDelete:
            return "The agent wants to delete the scene \"{}\"";
        case StrId::AgentConfirmWithData:
            return " and its data";
        case StrId::AgentConfirmFileRemove:
            return "The agent wants to delete a file of \"{}\": {}";
        case StrId::AgentNoteSetting:
            return "{} {} → {}";
        case StrId::AgentSetBrightness:
            return "Brightness";
        case StrId::AgentSetVolume:
            return "Volume";
        case StrId::AgentSetNotifyVol:
            return "Alert volume";
        case StrId::AgentSetScreenOff:
            return "Screen off after";
        case StrId::AgentSetLanguage:
            return "Language";
        case StrId::AgentConfirmTitle:
            return "Agent request";
        case StrId::AgentAllow:
            return "Allow";
        case StrId::AgentDeny:
            return "Deny";
        case StrId::AgentConfirmClear:
            return "Clear all agent conversations?";
        case StrId::AgentNoteMemoryAdded:
            return "Remembered a note.";
        case StrId::AgentNoteMemoryUpdated:
            return "Changed memory note #{}";
        case StrId::AgentNoteMemoryRemoved:
            return "Deleted memory note #{}";
        case StrId::AgentNoteTodoAdded:
            return "Added to-do \"{}\"";
        case StrId::AgentNoteTodoUpdated:
            return "Changed to-do \"{}\"";
        case StrId::AgentNoteTodoDone:
            return "Done: \"{}\"";
        case StrId::AgentNoteTodoRemoved:
            return "Deleted to-do \"{}\"";
        case StrId::NoteActionTodoDone:
            return "Done";
        case StrId::NoteActionTodoSnooze:
            return "In 10 min";
        case StrId::ReminderMissed:
            return "{} missed reminders";
        case StrId::HomeNoWifi:
            return "No Wi-Fi yet. Swipe right for Settings → Wi-Fi";
        case StrId::HomeNoLlm:
            return "No model set up yet. Set one on the admin page's Models tab";
        case StrId::HomeNotHeard:
            return "Didn't catch that";
        case StrId::VoiceErrNoAsr:
            return "No speech recognition set up. Set it on the admin page's Models tab";
        case StrId::VoiceErrNoTts:
            return "No speech voice set up. Set it on the admin page's Models tab";
        case StrId::VoiceErrOffline:
            return "Offline: no voice until the network is back";
        case StrId::VoiceErrAsr:
            return "Speech recognition failed. Please try again";
        case StrId::VoiceErrTts:
            return "Reading aloud failed; the text still shows";
        case StrId::VoiceErrMic:
            return "Microphone unavailable";
        case StrId::PageSettings:
            return "Settings";
        case StrId::PageScenes:
            return "Scenes";
        case StrId::PageHistory:
            return "History";
        case StrId::HistoryNew:
            return "New conversation";
        case StrId::HistoryNewConfirm:
            return "Start a new conversation? This one will be archived";
        case StrId::HistoryBusy:
            return "Agent is replying, try again shortly";
        case StrId::HistoryEmpty:
            return "No conversation yet. Hold the wisp to talk";
        case StrId::HistoryMore:
            return "Show details";
        case StrId::HistoryLess:
            return "Hide details";
        case StrId::BodyCode:
            return "Code · {} lines";
        case StrId::BodyCodeLang:
            return "{} code · {} lines";
        case StrId::SceneListEmpty:
            return "No scenes yet. Try saying 'make a pomodoro timer'";
        case StrId::SceneDeleteButton:
            return "Delete";
        case StrId::SceneDeleteData:
            return "Delete its data too";
        case StrId::SettingsBrightness:
            return "Brightness";
        case StrId::SettingsVolume:
            return "Volume";
        case StrId::SettingsScreenOff:
            return "Screen off after";
        case StrId::SettingsSeconds:
            return "{} s";
        case StrId::SettingsMinutes:
            return "{} min";
        case StrId::SettingsNever:
            return "Never";
        case StrId::SettingsTtsMute:
            return "Mute spoken replies";
        case StrId::SettingsWifi:
            return "Wi-Fi";
        case StrId::SettingsAbout:
            return "About";
        case StrId::AboutFirmware:
            return "Firmware";
        case StrId::AboutAddress:
            return "Admin page";
        case StrId::AboutAddressNone:
            return "Offline";
        case StrId::AboutPairHint:
            return "Open the admin page address in a browser and enter the pairing code shown on "
                   "the device when asked";
        case StrId::AboutDiagnostics:
            return "Hardware diagnostics";
        case StrId::TalkThinking:
            return "Thinking…";
        case StrId::ActivityStep:
            return "Step {} · {}";
        case StrId::ActivityDone:
            return "Steps: {} · {}";
        case StrId::ActivitySeconds:
            return "{} s";
        case StrId::ActivityMinSec:
            return "{} min {} s";
        case StrId::ActivityCall:
            return "Call {}";
        case StrId::ActivityMarkRunning:
            return "●";
        case StrId::ActivityMarkDone:
            return "✓";
        case StrId::ActivityMarkFailed:
            return "×";
        case StrId::ActSceneList:
            return "List scenes";
        case StrId::ActSceneListFiles:
            return "List scene files";
        case StrId::ActSceneReadFile:
            return "Read file";
        case StrId::ActSceneWriteFile:
            return "Write file";
        case StrId::ActSceneEditFile:
            return "Edit file";
        case StrId::ActSceneDeleteFile:
            return "Delete file";
        case StrId::ActSceneInstall:
            return "Install scene";
        case StrId::ActSceneDiscard:
            return "Discard draft";
        case StrId::ActSceneRollback:
            return "Roll back scene";
        case StrId::ActSceneOpen:
            return "Open scene";
        case StrId::ActSceneClose:
            return "Close scene";
        case StrId::ActSceneExit:
            return "Leave the scene";
        case StrId::ActSceneDelete:
            return "Delete scene";
        case StrId::ActSceneEval:
            return "Run code";
        case StrId::ActSceneLogs:
            return "Read logs";
        case StrId::ActSceneDataRead:
            return "Read scene data";
        case StrId::ActSceneDataWrite:
            return "Write scene data";
        case StrId::ActSceneFilesList:
            return "List stored files";
        case StrId::ActSceneFilesRead:
            return "Read stored file";
        case StrId::ActSceneFilesRemove:
            return "Remove stored file";
        case StrId::ActMemoryAdd:
            return "Remember something";
        case StrId::ActMemoryList:
            return "Check memories";
        case StrId::ActMemoryUpdate:
            return "Update memory";
        case StrId::ActMemoryRemove:
            return "Forget memory";
        case StrId::ActTodoAdd:
            return "Add to-do";
        case StrId::ActTodoList:
            return "Check to-dos";
        case StrId::ActTodoUpdate:
            return "Update to-do";
        case StrId::ActTodoDone:
            return "Complete to-do";
        case StrId::ActTodoRemove:
            return "Remove to-do";
        case StrId::ActSettingsGet:
            return "Check settings";
        case StrId::ActSettingsSet:
            return "Change a setting";
        case StrId::ActDeviceStatus:
            return "Check the device";
        case StrId::ActNotifyList:
            return "Check notifications";
        case StrId::ActNotifyDismiss:
            return "Dismiss a notification";
        case StrId::ActDocsRead:
            return "Read the docs";
        case StrId::ActUiTree:
            return "Inspect the screen";
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
