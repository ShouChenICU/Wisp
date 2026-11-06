#pragma once

// Device UI text in Chinese, Japanese and English (spec 8.3). Covers the
// built-in system screens only (the self-check screen and, later, the
// system cards and the main-scene shell); agent-generated scenes choose
// their own text.
//
// Every language's table is compiled in (strings_{zh,ja,en}.cpp). A
// string missing from the Japanese or English table falls back to
// Chinese. Every character any table uses must be in the built-in CJK
// font: tools/gen_cjk_font.py folds the tables' characters into the
// font's character set, and tests/ui/strings_test.cpp checks the result.
//
// Strings are fragments without printf conversions; callers compose them
// (e.g. "<Battery>: 85%"), so a translation can never mismatch a format.
// Where word order differs between languages a string carries one "{}"
// that tr_fill() replaces (every language has the same count, which
// tests/ui/strings_test.cpp checks).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wisp::ui {

enum class Lang : uint8_t { Zh, Ja, En };

inline constexpr Lang kAllLangs[] = {Lang::Zh, Lang::Ja, Lang::En};

enum class StrId : uint16_t {
    // Self-check header and device list.
    BootTime,
    StateOk,
    StateDegraded,
    StateMissing,
    StateFailed,

    // Self-check summary chips. SummaryDevices carries "{}" ("<ok>/<all>"),
    // SummaryTests "{}" ("<passed>/3").
    SummaryDevices,
    SummaryTests,

    // Self-check section card headings.
    SectionDevices,
    SectionNetwork,
    SectionSensors,
    SectionPowerClock,
    SectionStorage,
    SectionTests,
    SectionPerf,

    // Self-check page title and board card (BootTime is its boot-time
    // label).
    SelfCheckTitle,
    SectionBoard,
    BoardName,
    BoardTarget,
    Firmware,

    // The self-check page's language switcher: each language's own name,
    // the same in every table (never translated).
    LangNameEn,
    LangNameZh,
    LangNameJa,

    // Self-check readout labels.
    Battery,
    Time,
    TimeSync,
    InternalFlash,
    SdCard,
    Button,
    Touch,
    Accel,
    Gyro,
    TouchTest,
    PerfInternalRam,
    PerfPsram,

    // Self-check readout values.
    ReadFailed,
    LevelUnknown,
    Charging,
    NotCharging,
    Online,
    Offline,
    PerfFree,
    PerfLargestBlock,
    PerfTotal,
    PerfUsed,
    TouchTestArea,
    TestUntested,
    TestPassed,
    // The time sync row: TimeSynced carries "{}" (the local HH:MM of the
    // last sync).
    TimeSynced,
    TimeNotSynced,

    // Microphone loopback test.
    LoopbackButton,
    LoopReady,
    LoopRecording,
    LoopPlaying,
    LoopDone,
    LoopFailed,

    // Power-off confirmation dialog.
    PowerOffTitle,
    PowerOffPrompt,
    Yes,
    No,

    // Network section (connectivity spec 8.1). NetLine carries "{}" (one
    // of the states below), NetRetrying "{}" (seconds; see tr_fill()),
    // NetRejected "{}" (the SSID whose password was rejected);
    // NetRejectedNoName is the same sentence for when no SSID is known.
    NetLine,
    NetNotSetUp,
    NetOff,
    NetConnecting,
    NetRetrying,
    NetRejected,
    NetRejectedNoName,
    NetConnected,
    NetNoHardware,
    StartSetup,
    StopSetup,

    // Provisioning card. Hotspot and Password carry "{}" (the hotspot's
    // name / password), OpenAddress "{}" (the setup page URL).
    SetupTitle,
    Hotspot,
    Password,
    OpenAddress,

    // Pairing-code card. PairRemaining carries "{}" (seconds).
    PairTitle,
    PairRemaining,
    Reject,

    // Factory-reset confirmation dialog (answered with Yes/No).
    ResetTitle,
    ResetPrompt,

    // Firmware-update confirmation dialog (answered with Yes/No); the
    // screen puts "<current> → <new>" under FirmwarePrompt.
    FirmwareTitle,
    FirmwarePrompt,
    FirmwareDowngrade,
    // Scenes block of the self-check screen and scene states.
    Scenes,
    ScenesEmpty,
    SceneStateRunning,
    SceneStateError,
    // System layer: the scene error panel (spec 3.6) and the delete-scene
    // dialog (answered with Yes/No; SceneDeletePrompt carries "{}", the
    // scene's name, and SceneDeleteWithData follows it when the data goes
    // too). The keyboard's language key is not translated
    // (SystemLayer::kPinyinKey / kLatinKey).
    SceneErrorTitle,
    SceneReload,
    SceneBack,
    SceneDeleteTitle,
    SceneDeletePrompt,
    SceneDeleteWithData,

    // System notifications (scene-device spec 5.2), shown under
    // NoticeSystemTitle. Each body carries "{}": a scene's name, a wake
    // lock holder's label (the Holder* strings below, or a scene's name)
    // or the battery percentage. NoticeRelease is the wake lock notice's
    // action button.
    NoticeSystemTitle,
    NoticeEvictedMemory,
    NoticeEvictedCount,
    NoticeSceneError,
    NoticeRestoreFailed,
    NoticeDeleteFailed,
    NoticeWakeLock,
    NoticeBatteryLow,
    NoticeRelease,
    HolderAdmin,
    HolderUpload,
    HolderOta,
    HolderTimeSync,
    HolderNetWindow,
    HolderHotspot,
    HolderTone,
    HolderDownload,
    // The pull-down panel (spec 5.6). PanelMinutes and PanelHours carry
    // "{}", how long a scene has been in the background.
    PanelBackground,
    PanelNotifications,
    PanelClearAll,
    PanelEmpty,
    PanelJustNow,
    PanelMinutes,
    PanelHours,
    // Notes the agent adds to the conversation (agent-core spec 3.4).
    AgentStepLimit,
    AgentTimeLimit,
    AgentStopped,
    AgentInterrupted,
    AgentErrNoModel,
    AgentErrNetwork,
    AgentErrNetworkLost,
    AgentErrUnauthorized,
    AgentErrServer,
    AgentRolledOver,
    // The context grew past the model's window and its summary failed.
    AgentContextDropped,
    // The agent_done notice (agent-core spec 3.7): "{}" is the first line
    // of the reply.
    NoticeAgentDone,
    // The settings_not_saved notice: changed settings could not be saved.
    NoticeSettingsNotSaved,
    // Scene authoring notes (agent-core spec 4.1), "{}" the scene's name.
    // AgentNoteInstalled is followed by AgentVersionSuffix with the version.
    AgentNoteInstalled,
    AgentVersionSuffix,
    AgentNoteDiscarded,
    AgentNoteRolledBack,
    // The question before a scene gets new http hosts: followed by the
    // hosts, joined with AgentListSep.
    AgentConfirmHosts,
    AgentListSep,

    Count,  // not a string; keep last
};

inline constexpr size_t kStrCount = static_cast<size_t>(StrId::Count);

// The text for `id` in the current language (falling back to Chinese).
// Never null. Thread-safe -- the language is an atomic -- but UI text
// should only be built on the UI thread, so a screen never shows two
// languages at once.
const char* tr(StrId id);

// The text for `id` in `lang`, with the same Chinese fallback.
const char* tr(StrId id, Lang lang);

// tr(id) with its "{}" replaced by `value`; text without one comes back
// unchanged.
std::string tr_fill(StrId id, std::string_view value);

// The same in `lang`.
std::string tr_fill(StrId id, Lang lang, std::string_view value);

// Sets the language tr(StrId) uses. Call on the UI thread (or before any
// UI exists), then rebuild the visible screen's text.
void set_lang(Lang lang);
Lang lang();

// "zh" / "ja" / "en" (the Settings sys/lang values); anything else is
// nullopt.
std::optional<Lang> parse_lang(std::string_view code);
const char* lang_code(Lang lang);

namespace detail {
// The raw table entry for `id` in `lang`, or nullptr if that table has
// none -- no fallback. For tests that check every table is complete.
const char* table_text(StrId id, Lang lang);
}  // namespace detail

}  // namespace wisp::ui
