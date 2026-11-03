// English (en) device UI text (spec 8.3).

#include "strings_tables.hpp"

namespace wisp::ui::detail {

const char* strings_en(StrId id) {
    switch (id) {
        case StrId::BootTime:
            return "Boot";
        case StrId::DeviceList:
            return "Devices";
        case StrId::StateOk:
            return "OK";
        case StrId::StateDegraded:
            return "Degraded";
        case StrId::StateMissing:
            return "Missing";
        case StrId::StateFailed:
            return "Failed";
        case StrId::Battery:
            return "Battery";
        case StrId::Time:
            return "Time";
        case StrId::InternalFlash:
            return "Internal flash";
        case StrId::SdCard:
            return "SD card";
        case StrId::Button:
            return "Button";
        case StrId::Touch:
            return "Touch";
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
        case StrId::TouchTestArea:
            return "Touch test area";
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
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
