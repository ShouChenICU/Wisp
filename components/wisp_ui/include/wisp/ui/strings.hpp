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
// that tr_fill() replaces, or a few that tr_fill_n() replaces in order
// (every language has the same count, which tests/ui/strings_test.cpp
// checks).

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
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
    // name / password), OpenAddress "{}" (the setup page URL). Its success
    // view: SetupDoneTitle "{}" (the network joined), SetupDoneAddress "{}"
    // (the admin page URL), SetupDoneOk "{}" (seconds until it closes).
    SetupTitle,
    Hotspot,
    Password,
    OpenAddress,
    SetupDoneTitle,
    SetupDoneAddress,
    SetupDoneOk,

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
    // Notes the agent adds to the conversation (agent-core spec 3.4);
    // AgentTimeLimit "{}" is the turn's time limit in minutes.
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
    // Scene operation notes (agent-core spec 4.2), "{}" the scene's name;
    // AgentNoteDataWritten and AgentNoteFileRemoved have a second "{}",
    // the key or the file name (tr_fill_n()).
    AgentNoteOpened,
    AgentNoteClosed,
    AgentNoteToBackground,
    AgentNoteExited,
    AgentNoteDataWritten,
    AgentNoteEval,
    AgentNoteDeleted,
    AgentNoteFileRemoved,
    // Device confirmations (spec 6.1): deleting a scene, followed by
    // AgentConfirmWithData when its data goes too; removing one of its
    // files ("{}" the scene's name, then the file name).
    AgentConfirmDelete,
    AgentConfirmWithData,
    AgentConfirmFileRemove,
    // settings.set's note (spec 6.2): "{} {} → {}", the setting's name
    // (one of the AgentSet* below), the old value and the new one.
    AgentNoteSetting,
    AgentSetBrightness,
    AgentSetVolume,
    AgentSetNotifyVol,
    AgentSetScreenOff,
    AgentSetLanguage,
    AgentSetTimeFormat,
    // The device confirmation dialog (spec 6.1): its title, its buttons,
    // and the question before the whole conversation history is cleared.
    AgentConfirmTitle,
    AgentAllow,
    AgentDeny,
    AgentConfirmClear,
    // Memory notes (mvp spec 3.2, 5.1); AgentNoteMemoryUpdated and
    // AgentNoteMemoryRemoved carry the note's id as "{}".
    AgentNoteMemoryAdded,
    AgentNoteMemoryUpdated,
    AgentNoteMemoryRemoved,
    // Todos and their reminders (mvp spec 3.2, 5.2). The AgentNoteTodo*
    // notes carry the todo's title as "{}"; NoteActionTodoDone and
    // NoteActionTodoSnooze are a reminder's two buttons; ReminderMissed
    // titles the merged notice, "{}" the count.
    AgentNoteTodoAdded,
    AgentNoteTodoUpdated,
    AgentNoteTodoDone,
    AgentNoteTodoRemoved,
    NoteActionTodoDone,
    NoteActionTodoSnooze,
    ReminderMissed,
    // The home screen's hint below the wisp (mvp spec 4.6, 6.2, 6.6):
    // what is not set up yet, the last voice error, or "not heard". The
    // screen adds the admin address on a line of its own after HomeNoLlm,
    // VoiceErrNoAsr and VoiceErrNoTts.
    HomeNoWifi,
    HomeNoLlm,
    HomeNotHeard,
    VoiceErrNoAsr,
    VoiceErrNoTts,
    VoiceErrOffline,
    VoiceErrAsr,
    VoiceErrTts,
    VoiceErrMic,
    // The shell's sub-pages' titles.
    PageSettings,
    PageScenes,
    PageHistory,
    // The history page (mvp spec 6.4): the button at its bottom, its
    // confirmation, what shows while the agent is busy or nothing was said
    // yet, and the toggle of a turn's folded body.
    HistoryNew,
    HistoryNewConfirm,
    HistoryBusy,
    HistoryEmpty,
    HistoryMore,
    HistoryLess,
    // A reply's fenced code block, folded to one line on the home screen
    // and the history page (mvp spec 6.2): BodyCode "{}" is its line
    // count, BodyCodeLang "{}", "{}" the fence's language tag and the
    // count.
    BodyCode,
    BodyCodeLang,
    // The scene list (mvp spec 6.4): the hint of an empty list and the
    // long-press card.
    SceneListEmpty,
    SceneDeleteButton,
    SceneDeleteData,
    // The settings page (mvp spec 6.4). SettingsSeconds and
    // SettingsMinutes carry the number as "{}"; AboutAddressNone stands
    // for the admin address while there is none.
    SettingsBrightness,
    SettingsVolume,
    SettingsScreenOff,
    SettingsSeconds,
    SettingsMinutes,
    SettingsNever,
    SettingsWifi,
    AboutFirmware,
    AboutAddress,
    AboutAddressNone,
    AboutPairHint,
    AboutDiagnostics,
    // A 12-hour clock's day halves around "H:MM" (wisp/ui/clock_text.hpp).
    ClockAm,
    ClockPm,
    // The settings page's groups and rows (mvp spec 6.4). SettingsHours
    // carries the number as "{}". LangFull* name each language in itself,
    // the same in every table. TzFormat "{} {}" is a time zone choice's
    // "UTC+08:00" and its city, one Tz* per offset of the choice list
    // (TzEast0800: UTC+08:00, TzWest0330: UTC-03:30).
    SettingsGroupDisplay,
    SettingsGroupPower,
    SettingsGroupNetwork,
    SettingsGroupLangTime,
    SettingsGroupSystem,
    SettingsNotifyVol,
    SettingsSpeak,
    SettingsDeepStandby,
    SettingsHours,
    SettingsLightSleep,
    SettingsLightSleepHint,
    SettingsLanguage,
    SettingsTimeZone,
    SettingsTimeFormat,
    SettingsClock24,
    SettingsClock12,
    SettingsRestart,
    SettingsPowerOff,
    RestartTitle,
    RestartPrompt,
    LangFullZh,
    LangFullJa,
    LangFullEn,
    TzFormat,
    TzWest1200,
    TzWest1100,
    TzWest1000,
    TzWest0930,
    TzWest0900,
    TzWest0800,
    TzWest0700,
    TzWest0600,
    TzWest0500,
    TzWest0400,
    TzWest0330,
    TzWest0300,
    TzWest0200,
    TzWest0100,
    TzEast0000,
    TzEast0100,
    TzEast0200,
    TzEast0300,
    TzEast0330,
    TzEast0400,
    TzEast0430,
    TzEast0500,
    TzEast0530,
    TzEast0545,
    TzEast0600,
    TzEast0630,
    TzEast0700,
    TzEast0800,
    TzEast0845,
    TzEast0900,
    TzEast0930,
    TzEast1000,
    TzEast1030,
    TzEast1100,
    TzEast1200,
    TzEast1245,
    TzEast1300,
    TzEast1400,
    // The talk overlay (mvp spec 6.5): shown over a scene while holding to
    // talk, once the capture has ended and the agent has not replied yet.
    TalkThinking,
    // The agent's steps on the home screen (mvp spec 6.2;
    // wisp/shell/activity.hpp). ActivityStep "{} · {}" is the running
    // step's number and the time so far, ActivityDone the steps a turn
    // took and its time, each time ActivitySeconds or ActivityMinSec;
    // ActivityCall "{}" names a tool without a label of its own. The
    // three marks are the steps' status glyphs, the same in every table.
    // The Act* labels name one tool each; the screen adds the step's
    // target after a space.
    ActivityStep,
    ActivityDone,
    ActivitySeconds,
    ActivityMinSec,
    ActivityCall,
    ActivityMarkRunning,
    ActivityMarkDone,
    ActivityMarkFailed,
    ActSceneList,
    ActSceneListFiles,
    ActSceneReadFile,
    ActSceneWriteFile,
    ActSceneEditFile,
    ActSceneDeleteFile,
    ActSceneInstall,
    ActSceneDiscard,
    ActSceneRollback,
    ActSceneOpen,
    ActSceneClose,
    ActSceneExit,
    ActSceneDelete,
    ActSceneEval,
    ActSceneLogs,
    ActSceneDataRead,
    ActSceneDataWrite,
    ActSceneFilesList,
    ActSceneFilesRead,
    ActSceneFilesRemove,
    ActMemoryAdd,
    ActMemoryList,
    ActMemoryUpdate,
    ActMemoryRemove,
    ActTodoAdd,
    ActTodoList,
    ActTodoUpdate,
    ActTodoDone,
    ActTodoRemove,
    ActSettingsGet,
    ActSettingsSet,
    ActDeviceStatus,
    ActNotifyList,
    ActNotifyDismiss,
    ActDocsRead,
    ActUiTree,

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

// tr(id, lang) with each "{}" in turn replaced by the next of `values`;
// placeholders past the last value stay as they are, and the values
// themselves are never searched for placeholders.
std::string tr_fill_n(StrId id, Lang lang, std::initializer_list<std::string_view> values);
std::string tr_fill_n(StrId id, Lang lang, std::span<const std::string_view> values);

// The same in the current language.
std::string tr_fill_n(StrId id, std::span<const std::string_view> values);

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
