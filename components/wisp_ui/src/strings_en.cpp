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
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
