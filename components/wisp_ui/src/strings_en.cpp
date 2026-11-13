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
            return "Over {} minutes; stopped";
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
        case StrId::AgentNoteFetch:
            return "Fetched {}";
        case StrId::AgentNoteLuaRun:
            return "Ran a Lua script";
        case StrId::AgentNoteToolCall:
            return "Call {} · {}";
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
        case StrId::AgentSetTimeFormat:
            return "Time format";
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
        case StrId::SettingsWifi:
            return "Wi-Fi";
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
        case StrId::ClockAm:
            return "{} AM";
        case StrId::ClockPm:
            return "{} PM";
        case StrId::SettingsGroupDisplay:
            return "Display & sound";
        case StrId::SettingsGroupPower:
            return "Screen & power";
        case StrId::SettingsGroupModels:
            return "Models";
        case StrId::ModelLlm:
            return "Chat model";
        case StrId::ModelAsr:
            return "Speech recognition";
        case StrId::ModelTts:
            return "Speech synthesis";
        case StrId::ModelNone:
            return "Not configured";
        case StrId::ModelCurrent:
            return "Active";
        case StrId::ModelTest:
            return "Test connection";
        case StrId::ModelActivate:
            return "Activate";
        case StrId::ModelTestOk:
            return "Connection successful";
        case StrId::ModelTestTimeout:
            return "Connection timed out";
        case StrId::ModelTestNetwork:
            return "Cannot reach the service. Check the network.";
        case StrId::ModelTestTls:
            return "Secure connection failed. Check the service URL and certificate.";
        case StrId::ModelTestAuth:
            return "Authentication failed. Check the key in the admin console.";
        case StrId::ModelTestNoList:
            return "The service does not provide a model list";
        case StrId::ModelTestHttp:
            return "Unexpected service response";
        case StrId::WifiSecured:
            return "Secured network";
        case StrId::WifiOpen:
            return "Open network";
        case StrId::WifiNotFound:
            return "Network not found. Scan again.";
        case StrId::ModelTestFailed:
            return "Connection failed. Check the admin console.";
        case StrId::ModelActivated:
            return "Activated";
        case StrId::ModelManageHint:
            return "Add or edit providers in the browser admin console. Connection tests check the "
                   "service and model list.";
        case StrId::ConnectionBusy:
            return "Working…";
        case StrId::ConnectionFailed:
            return "Operation failed. Try again.";
        case StrId::ConnectionUnavailable:
            return "Unavailable";
        case StrId::ModelManage:
            return "Manage providers";
        case StrId::WifiNearby:
            return "Nearby networks";
        case StrId::WifiStrong:
            return "Strong signal";
        case StrId::WifiWeak:
            return "Weak signal";
        case StrId::WifiShowPassword:
            return "Show";
        case StrId::WifiDisconnect:
            return "Disconnect";
        case StrId::WifiChannel:
            return "Channel";
        case StrId::WifiBssid:
            return "Access point (BSSID)";
        case StrId::WifiSubnet:
            return "Subnet mask";
        case StrId::WifiGateway:
            return "Router";
        case StrId::WifiDns:
            return "DNS server";
        case StrId::WifiMac:
            return "Device address (MAC)";
        case StrId::WifiIpAddress:
            return "IP address";
        case StrId::WifiSignal:
            return "Signal";
        case StrId::WifiSecurity:
            return "Security";
        case StrId::WifiDetails:
            return "Details";
        case StrId::WifiScanning:
            return "Scanning…";
        case StrId::WifiPasswordLength:
            return "8-63 chars";
        case StrId::WifiHidePassword:
            return "Hide";
        case StrId::WifiEnabled:
            return "Wi-Fi";
        case StrId::WifiScan:
            return "Scan networks";
        case StrId::WifiSetupPhone:
            return "Set up with phone";
        case StrId::WifiSaved:
            return "Saved";
        case StrId::WifiAuthFailed:
            return "Incorrect password. Enter it again.";
        case StrId::WifiConnect:
            return "Connect";
        case StrId::WifiPassword:
            return "Password";
        case StrId::WifiChangePassword:
            return "Re-enter password";
        case StrId::WifiPasswordInvalid:
            return "Password must be 8-63 characters";
        case StrId::WifiCancel:
            return "Cancel";
        case StrId::WifiOff:
            return "Wi-Fi is off";
        case StrId::WifiConnected:
            return "Connected";
        case StrId::WifiConnecting:
            return "Connecting";
        case StrId::WifiDisconnected:
            return "Disconnected";
        case StrId::WifiEmpty:
            return "No networks found. Scan again or set up with your phone.";
        case StrId::WifiScanFailed:
            return "Scan failed. Try again.";
        case StrId::WifiScheduled:
            return "Connection started";
        case StrId::SettingsGroupNetwork:
            return "Network";
        case StrId::SettingsGroupLangTime:
            return "Language & time";
        case StrId::SettingsGroupSystem:
            return "System";
        case StrId::SettingsNotifyVol:
            return "Alert volume";
        case StrId::SettingsSpeak:
            return "Spoken replies";
        case StrId::SettingsDeepStandby:
            return "Deep standby";
        case StrId::SettingsHours:
            return "{} h";
        case StrId::SettingsLanguage:
            return "Language";
        case StrId::SettingsTimeZone:
            return "Time zone";
        case StrId::SettingsTimeFormat:
            return "Time format";
        case StrId::SettingsClock24:
            return "24-hour";
        case StrId::SettingsClock12:
            return "12-hour";
        case StrId::SettingsRestart:
            return "Restart";
        case StrId::SettingsPowerOff:
            return "Power off";
        case StrId::RestartTitle:
            return "Restart";
        case StrId::RestartPrompt:
            return "Restart the device?";
        case StrId::LangFullZh:
            return "简体中文";
        case StrId::LangFullJa:
            return "日本語";
        case StrId::LangFullEn:
            return "English";
        case StrId::TzFormat:
            return "{} {}";
        case StrId::TzWest1200:
            return "Baker Island";
        case StrId::TzWest1100:
            return "Midway";
        case StrId::TzWest1000:
            return "Honolulu";
        case StrId::TzWest0930:
            return "Marquesas";
        case StrId::TzWest0900:
            return "Anchorage";
        case StrId::TzWest0800:
            return "Los Angeles";
        case StrId::TzWest0700:
            return "Denver";
        case StrId::TzWest0600:
            return "Chicago";
        case StrId::TzWest0500:
            return "New York";
        case StrId::TzWest0400:
            return "Caracas";
        case StrId::TzWest0330:
            return "St. John's";
        case StrId::TzWest0300:
            return "Sao Paulo";
        case StrId::TzWest0200:
            return "South Georgia";
        case StrId::TzWest0100:
            return "Azores";
        case StrId::TzEast0000:
            return "London";
        case StrId::TzEast0100:
            return "Paris";
        case StrId::TzEast0200:
            return "Cairo";
        case StrId::TzEast0300:
            return "Moscow";
        case StrId::TzEast0330:
            return "Tehran";
        case StrId::TzEast0400:
            return "Dubai";
        case StrId::TzEast0430:
            return "Kabul";
        case StrId::TzEast0500:
            return "Karachi";
        case StrId::TzEast0530:
            return "New Delhi";
        case StrId::TzEast0545:
            return "Kathmandu";
        case StrId::TzEast0600:
            return "Dhaka";
        case StrId::TzEast0630:
            return "Yangon";
        case StrId::TzEast0700:
            return "Bangkok";
        case StrId::TzEast0800:
            return "Beijing";
        case StrId::TzEast0845:
            return "Eucla";
        case StrId::TzEast0900:
            return "Tokyo";
        case StrId::TzEast0930:
            return "Adelaide";
        case StrId::TzEast1000:
            return "Sydney";
        case StrId::TzEast1030:
            return "Lord Howe";
        case StrId::TzEast1100:
            return "Noumea";
        case StrId::TzEast1200:
            return "Auckland";
        case StrId::TzEast1245:
            return "Chatham Islands";
        case StrId::TzEast1300:
            return "Tonga";
        case StrId::TzEast1400:
            return "Line Islands";
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
        case StrId::ActHttpFetch:
            return "Fetch web page";
        case StrId::ActLuaRun:
            return "Run Lua script";
        case StrId::ActToolsFind:
            return "Find scene tools";
        case StrId::ActToolsCall:
            return "Call scene tool";
        case StrId::ActUiTree:
            return "Inspect the screen";
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
