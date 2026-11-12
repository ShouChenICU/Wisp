// Japanese (ja) device UI text (spec 8.3). Kanji render with the built-in
// font's Simplified Chinese glyph shapes until a Japanese-shaped font can
// be loaded per language (spec 8.3).

#include "strings_tables.hpp"

namespace wisp::ui::detail {

const char* strings_ja(StrId id) {
    switch (id) {
        case StrId::BootTime:
            return "起動";
        case StrId::StateOk:
            return "正常";
        case StrId::StateDegraded:
            return "機能制限";
        case StrId::StateMissing:
            return "未検出";
        case StrId::StateFailed:
            return "エラー";
        case StrId::SummaryDevices:
            return "デバイス {} 正常";
        case StrId::SummaryTests:
            return "テスト {} 完了";
        case StrId::SectionDevices:
            return "デバイス";
        case StrId::SectionNetwork:
            return "ネットワーク";
        case StrId::SectionSensors:
            return "センサー";
        case StrId::SectionPowerClock:
            return "電源と時計";
        case StrId::SectionStorage:
            return "ストレージ";
        case StrId::SectionTests:
            return "操作テスト";
        case StrId::SectionPerf:
            return "パフォーマンス";
        case StrId::SelfCheckTitle:
            return "セルフチェック";
        case StrId::SectionBoard:
            return "ボード情報";
        case StrId::BoardName:
            return "ボード";
        case StrId::BoardTarget:
            return "ターゲット";
        case StrId::Firmware:
            return "ファームウェア";
        case StrId::LangNameEn:
            return "EN";
        case StrId::LangNameZh:
            return "中文";
        case StrId::LangNameJa:
            return "日本語";
        case StrId::Battery:
            return "バッテリー";
        case StrId::Time:
            return "時刻";
        case StrId::TimeSync:
            return "時刻同期";
        case StrId::InternalFlash:
            return "内蔵フラッシュ";
        case StrId::SdCard:
            return "SDカード";
        case StrId::Button:
            return "ボタン";
        case StrId::Touch:
            return "タッチ";
        case StrId::Accel:
            return "加速度";
        case StrId::Gyro:
            return "角速度";
        case StrId::TouchTest:
            return "タッチ";
        case StrId::PerfInternalRam:
            return "内部RAM";
        case StrId::PerfPsram:
            return "PSRAM";
        case StrId::ReadFailed:
            return "読み取りエラー";
        case StrId::LevelUnknown:
            return "残量不明";
        case StrId::Charging:
            return "充電中";
        case StrId::NotCharging:
            return "充電なし";
        case StrId::Online:
            return "オンライン";
        case StrId::Offline:
            return "オフライン";
        case StrId::PerfFree:
            return "空き";
        case StrId::PerfLargestBlock:
            return "最大ブロック";
        case StrId::PerfTotal:
            return "合計";
        case StrId::PerfUsed:
            return "使用";
        case StrId::TouchTestArea:
            return "タッチテスト";
        case StrId::TestUntested:
            return "未テスト";
        case StrId::TestPassed:
            return "合格";
        case StrId::TimeSynced:
            return "同期済み {}";
        case StrId::TimeNotSynced:
            return "未同期";
        case StrId::LoopbackButton:
            return "マイクテスト";
        case StrId::LoopReady:
            return "待機中";
        case StrId::LoopRecording:
            return "録音中";
        case StrId::LoopPlaying:
            return "再生中";
        case StrId::LoopDone:
            return "完了";
        case StrId::LoopFailed:
            return "エラー";
        case StrId::PowerOffTitle:
            return "電源オフ";
        case StrId::PowerOffPrompt:
            return "電源を切りますか？";
        case StrId::Yes:
            return "はい";
        case StrId::No:
            return "いいえ";
        case StrId::NetLine:
            return "Wi-Fi：{}";
        case StrId::NetNotSetUp:
            return "未設定";
        case StrId::NetOff:
            return "未接続";
        case StrId::NetConnecting:
            return "接続中…";
        case StrId::NetRetrying:
            return "再試行（{} 秒後）";
        case StrId::NetRejected:
            return "{} のパスワードが拒否されました。再設定してください";
        case StrId::NetRejectedNoName:
            return "パスワードが拒否されました。再設定してください";
        case StrId::NetConnected:
            return "接続済み";
        case StrId::NetNoHardware:
            return "Wi-Fi なし";
        case StrId::StartSetup:
            return "Wi-Fi 設定を開始";
        case StrId::StopSetup:
            return "Wi-Fi 設定を終了";
        case StrId::SetupTitle:
            return "Wi-Fi 設定";
        case StrId::Hotspot:
            return "ネットワーク：{}";
        case StrId::Password:
            return "パスワード：{}";
        case StrId::OpenAddress:
            return "接続後に {} を開く";
        case StrId::SetupDoneTitle:
            return "{} に接続しました";
        case StrId::SetupDoneAddress:
            return "管理画面：{}";
        case StrId::SetupDoneOk:
            return "OK（{}）";
        case StrId::PairTitle:
            return "ペアリング要求";
        case StrId::PairRemaining:
            return "残り {} 秒";
        case StrId::Reject:
            return "拒否";
        case StrId::ResetTitle:
            return "初期化";
        case StrId::ResetPrompt:
            return "Wi-"
                   "Fi・ペアリング・モデル設定を消去して再起動します。シーンのデータは残ります。";
        case StrId::FirmwareTitle:
            return "ファームウェア更新";
        case StrId::FirmwarePrompt:
            return "新しいファームウェアを入れて再起動しますか？";
        case StrId::FirmwareDowngrade:
            return "注意：今より古い版です（ダウングレード）。";
        case StrId::Scenes:
            return "シーン";
        case StrId::ScenesEmpty:
            return "シーンはまだありません";
        case StrId::SceneStateRunning:
            return "実行中";
        case StrId::SceneStateError:
            return "エラー";
        case StrId::SceneErrorTitle:
            return "シーンのエラー";
        case StrId::SceneReload:
            return "再読み込み";
        case StrId::SceneBack:
            return "戻る";
        case StrId::SceneDeleteTitle:
            return "シーンの削除";
        case StrId::SceneDeletePrompt:
            return "シーン「{}」を削除しますか？";
        case StrId::SceneDeleteWithData:
            return "シーンのデータも削除されます。";
        case StrId::NoticeSystemTitle:
            return "システム";
        case StrId::NoticeEvictedMemory:
            return "{}はメモリ不足のため終了しました";
        case StrId::NoticeEvictedCount:
            return "{}を終了しました：バックグラウンドが上限です";
        case StrId::NoticeSceneError:
            return "{}でエラーが発生し、停止しました";
        case StrId::NoticeRestoreFailed:
            return "{}を起動時に復元できませんでした";
        case StrId::NoticeDeleteFailed:
            return "{}を削除できませんでした。もう一度お試しください";
        case StrId::NoticeWakeLock:
            return "{}がデバイスを 30 分間起動したままにしています";
        case StrId::NoticeBatteryLow:
            return "バッテリー残量低下：{}%";
        case StrId::NoticeRelease:
            return "解除";
        case StrId::HolderUpload:
            return "アップロード";
        case StrId::HolderOta:
            return "ファームウェア更新";
        case StrId::HolderTimeSync:
            return "時刻同期";
        case StrId::HolderNetWindow:
            return "バックグラウンド通信";
        case StrId::HolderHotspot:
            return "設定用ホットスポット";
        case StrId::HolderTone:
            return "通知音";
        case StrId::HolderDownload:
            return "ファイルのダウンロード";
        case StrId::PanelBackground:
            return "バックグラウンド";
        case StrId::PanelNotifications:
            return "通知";
        case StrId::PanelClearAll:
            return "すべて消去";
        case StrId::PanelEmpty:
            return "通知はありません";
        case StrId::PanelJustNow:
            return "たった今";
        case StrId::PanelMinutes:
            return "{} 分";
        case StrId::PanelHours:
            return "{} 時間";
        case StrId::AgentStepLimit:
            return "ステップが多すぎるため停止しました";
        case StrId::AgentTimeLimit:
            return "{} 分を超えたため停止しました";
        case StrId::AgentStopped:
            return "中止しました";
        case StrId::AgentInterrupted:
            return "中断されました";
        case StrId::AgentErrNoModel:
            return "使えるモデルがありません。管理画面の「モデル」ページで追加して有効にしてくださ"
                   "い";
        case StrId::AgentErrNetwork:
            return "ネットワークに接続できません。Wi-Fi を確認してください";
        case StrId::AgentErrNetworkLost:
            return "ネットワークが切断されました";
        case StrId::AgentErrUnauthorized:
            return "モデルが API Key "
                   "を拒否しました。管理画面の「モデル」ページで設定を確認してください";
        case StrId::AgentErrServer:
            return "モデルサービスでエラーが発生しました。しばらくしてからお試しください";
        case StrId::AgentRolledOver:
            return "会話が長くなったため、保存して新しい会話を始めました";
        case StrId::AgentContextDropped:
            return "会話が長くなり、古い部分を要約できなかったため、文脈から省きました（記録は残っ"
                   "ています）";
        case StrId::NoticeAgentDone:
            return "完了：{}";
        case StrId::NoticeSettingsNotSaved:
            return "設定を保存できませんでした";
        case StrId::AgentNoteInstalled:
            return "「{}」をインストールしました";
        case StrId::AgentVersionSuffix:
            return "（{}）";
        case StrId::AgentNoteDiscarded:
            return "「{}」の下書きを破棄しました";
        case StrId::AgentNoteRolledBack:
            return "「{}」を前のバージョンに戻しました";
        case StrId::AgentConfirmHosts:
            return "「{}」が次のサイトにアクセスできるようになります：";
        case StrId::AgentListSep:
            return "、";
        case StrId::AgentNoteOpened:
            return "「{}」を開きました";
        case StrId::AgentNoteClosed:
            return "「{}」を閉じました";
        case StrId::AgentNoteToBackground:
            return "「{}」をバックグラウンドに移しました";
        case StrId::AgentNoteExited:
            return "「{}」を終了しました";
        case StrId::AgentNoteDataWritten:
            return "「{}」のデータ {} を書き換えました";
        case StrId::AgentNoteEval:
            return "「{}」で Lua を実行しました";
        case StrId::AgentNoteDeleted:
            return "「{}」を削除しました";
        case StrId::AgentNoteFileRemoved:
            return "「{}」のファイル {} を削除しました";
        case StrId::AgentNoteFetch:
            return "{} にアクセス";
        case StrId::AgentNoteLuaRun:
            return "Lua スクリプトを実行";
        case StrId::AgentNoteToolCall:
            return "「{}」を呼び出し · {}";
        case StrId::AgentConfirmDelete:
            return "Agent がシーン「{}」を削除しようとしています";
        case StrId::AgentConfirmWithData:
            return "（データも含む）";
        case StrId::AgentConfirmFileRemove:
            return "Agent が「{}」のファイル {} を削除しようとしています";
        case StrId::AgentNoteSetting:
            return "{} {} → {}";
        case StrId::AgentSetBrightness:
            return "明るさ";
        case StrId::AgentSetVolume:
            return "音量";
        case StrId::AgentSetNotifyVol:
            return "通知音量";
        case StrId::AgentSetScreenOff:
            return "画面オフまでの時間";
        case StrId::AgentSetLanguage:
            return "言語";
        case StrId::AgentSetTimeFormat:
            return "時刻の表示";
        case StrId::AgentConfirmTitle:
            return "Agent の確認";
        case StrId::AgentAllow:
            return "許可";
        case StrId::AgentDeny:
            return "拒否";
        case StrId::AgentConfirmClear:
            return "Agent との会話をすべて消去しますか？";
        case StrId::AgentNoteMemoryAdded:
            return "メモを記憶しました";
        case StrId::AgentNoteMemoryUpdated:
            return "メモ #{} を修正しました";
        case StrId::AgentNoteMemoryRemoved:
            return "メモ #{} を削除しました";
        case StrId::AgentNoteTodoAdded:
            return "ToDo「{}」を追加しました";
        case StrId::AgentNoteTodoUpdated:
            return "ToDo「{}」を変更しました";
        case StrId::AgentNoteTodoDone:
            return "「{}」を完了しました";
        case StrId::AgentNoteTodoRemoved:
            return "ToDo「{}」を削除しました";
        case StrId::NoteActionTodoDone:
            return "完了";
        case StrId::NoteActionTodoSnooze:
            return "10 分後";
        case StrId::ReminderMissed:
            return "{} 件のリマインダーを見逃しました";
        case StrId::HomeNoWifi:
            return "Wi-Fi 未接続です。右にスワイプして 設定 → Wi-Fi";
        case StrId::HomeNoLlm:
            return "モデルが未設定です。管理画面のモデルページで設定してください";
        case StrId::HomeNotHeard:
            return "聞き取れませんでした";
        case StrId::VoiceErrNoAsr:
            return "音声認識が未設定です。管理画面のモデルページで設定してください";
        case StrId::VoiceErrNoTts:
            return "音声合成が未設定です。管理画面のモデルページで設定してください";
        case StrId::VoiceErrOffline:
            return "ネットワークに接続されていません";
        case StrId::VoiceErrAsr:
            return "音声認識に失敗しました。もう一度どうぞ";
        case StrId::VoiceErrTts:
            return "読み上げに失敗しました。文字はそのまま表示します";
        case StrId::VoiceErrMic:
            return "マイクが使えません";
        case StrId::PageSettings:
            return "設定";
        case StrId::PageScenes:
            return "シーン";
        case StrId::PageHistory:
            return "会話履歴";
        case StrId::HistoryNew:
            return "新しい会話";
        case StrId::HistoryNewConfirm:
            return "新しい会話を始めますか？今の会話はアーカイブされます";
        case StrId::HistoryBusy:
            return "Agent が返信中です。少し待ってから試してください";
        case StrId::HistoryEmpty:
            return "まだ会話がありません。鬼火を長押しして話しかけてください";
        case StrId::HistoryMore:
            return "詳細を開く";
        case StrId::HistoryLess:
            return "詳細を閉じる";
        case StrId::BodyCode:
            return "コード · {} 行";
        case StrId::BodyCodeLang:
            return "{} コード · {} 行";
        case StrId::SceneListEmpty:
            return "まだシーンがありません。「ポモドーロタイマーを作って」と話しかけてみてください";
        case StrId::SceneDeleteButton:
            return "削除";
        case StrId::SceneDeleteData:
            return "データも削除";
        case StrId::SettingsBrightness:
            return "明るさ";
        case StrId::SettingsVolume:
            return "音量";
        case StrId::SettingsScreenOff:
            return "自動消灯";
        case StrId::SettingsSeconds:
            return "{} 秒";
        case StrId::SettingsMinutes:
            return "{} 分";
        case StrId::SettingsNever:
            return "しない";
        case StrId::SettingsWifi:
            return "Wi-Fi";
        case StrId::AboutFirmware:
            return "ファームウェア";
        case StrId::AboutAddress:
            return "管理画面";
        case StrId::AboutAddressNone:
            return "未接続";
        case StrId::AboutPairHint:
            return "ブラウザで管理画面のアドレスを開き、案内に従って端末に表示されるペアリングコー"
                   "ドを入力してください";
        case StrId::AboutDiagnostics:
            return "ハードウェア診断";
        case StrId::ClockAm:
            return "午前{}";
        case StrId::ClockPm:
            return "午後{}";
        case StrId::SettingsGroupDisplay:
            return "画面とサウンド";
        case StrId::SettingsGroupPower:
            return "スリープと電源";
        case StrId::SettingsGroupNetwork:
            return "ネットワーク";
        case StrId::SettingsGroupLangTime:
            return "言語と時刻";
        case StrId::SettingsGroupSystem:
            return "システム";
        case StrId::SettingsNotifyVol:
            return "通知音量";
        case StrId::SettingsSpeak:
            return "音声読み上げ";
        case StrId::SettingsDeepStandby:
            return "ディープスタンバイ";
        case StrId::SettingsHours:
            return "{} 時間";
        case StrId::SettingsLightSleep:
            return "省電力スリープ";
        case StrId::SettingsLightSleepHint:
            return "ディープスタンバイ中はチップを浅く眠らせて節電します";
        case StrId::SettingsLanguage:
            return "言語";
        case StrId::SettingsTimeZone:
            return "タイムゾーン";
        case StrId::SettingsTimeFormat:
            return "時刻の表示";
        case StrId::SettingsClock24:
            return "24 時間表示";
        case StrId::SettingsClock12:
            return "12 時間表示";
        case StrId::SettingsRestart:
            return "再起動";
        case StrId::SettingsPowerOff:
            return "電源を切る";
        case StrId::RestartTitle:
            return "再起動";
        case StrId::RestartPrompt:
            return "デバイスを再起動しますか？";
        case StrId::LangFullZh:
            return "简体中文";
        case StrId::LangFullJa:
            return "日本語";
        case StrId::LangFullEn:
            return "English";
        case StrId::TzFormat:
            return "{} {}";
        case StrId::TzWest1200:
            return "ベーカー島";
        case StrId::TzWest1100:
            return "ミッドウェー";
        case StrId::TzWest1000:
            return "ホノルル";
        case StrId::TzWest0930:
            return "マルキーズ諸島";
        case StrId::TzWest0900:
            return "アンカレッジ";
        case StrId::TzWest0800:
            return "ロサンゼルス";
        case StrId::TzWest0700:
            return "デンバー";
        case StrId::TzWest0600:
            return "シカゴ";
        case StrId::TzWest0500:
            return "ニューヨーク";
        case StrId::TzWest0400:
            return "カラカス";
        case StrId::TzWest0330:
            return "セントジョンズ";
        case StrId::TzWest0300:
            return "サンパウロ";
        case StrId::TzWest0200:
            return "サウスジョージア";
        case StrId::TzWest0100:
            return "アゾレス諸島";
        case StrId::TzEast0000:
            return "ロンドン";
        case StrId::TzEast0100:
            return "パリ";
        case StrId::TzEast0200:
            return "カイロ";
        case StrId::TzEast0300:
            return "モスクワ";
        case StrId::TzEast0330:
            return "テヘラン";
        case StrId::TzEast0400:
            return "ドバイ";
        case StrId::TzEast0430:
            return "カブール";
        case StrId::TzEast0500:
            return "カラチ";
        case StrId::TzEast0530:
            return "ニューデリー";
        case StrId::TzEast0545:
            return "カトマンズ";
        case StrId::TzEast0600:
            return "ダッカ";
        case StrId::TzEast0630:
            return "ヤンゴン";
        case StrId::TzEast0700:
            return "バンコク";
        case StrId::TzEast0800:
            return "北京";
        case StrId::TzEast0845:
            return "ユークラ";
        case StrId::TzEast0900:
            return "東京";
        case StrId::TzEast0930:
            return "アデレード";
        case StrId::TzEast1000:
            return "シドニー";
        case StrId::TzEast1030:
            return "ロード・ハウ島";
        case StrId::TzEast1100:
            return "ヌーメア";
        case StrId::TzEast1200:
            return "オークランド";
        case StrId::TzEast1245:
            return "チャタム諸島";
        case StrId::TzEast1300:
            return "トンガ";
        case StrId::TzEast1400:
            return "ライン諸島";
        case StrId::TalkThinking:
            return "考え中…";
        case StrId::ActivityStep:
            return "ステップ {} · {}";
        case StrId::ActivityDone:
            return "{} ステップ · {}";
        case StrId::ActivitySeconds:
            return "{} 秒";
        case StrId::ActivityMinSec:
            return "{} 分 {} 秒";
        case StrId::ActivityCall:
            return "{} を呼び出し";
        case StrId::ActivityMarkRunning:
            return "●";
        case StrId::ActivityMarkDone:
            return "✓";
        case StrId::ActivityMarkFailed:
            return "×";
        case StrId::ActSceneList:
            return "シーン一覧を確認";
        case StrId::ActSceneListFiles:
            return "シーンのファイルを確認";
        case StrId::ActSceneReadFile:
            return "ファイルを読む";
        case StrId::ActSceneWriteFile:
            return "ファイルを書く";
        case StrId::ActSceneEditFile:
            return "ファイルを編集";
        case StrId::ActSceneDeleteFile:
            return "ファイルを削除";
        case StrId::ActSceneInstall:
            return "シーンをインストール";
        case StrId::ActSceneDiscard:
            return "下書きを破棄";
        case StrId::ActSceneRollback:
            return "シーンを元に戻す";
        case StrId::ActSceneOpen:
            return "シーンを開く";
        case StrId::ActSceneClose:
            return "シーンを閉じる";
        case StrId::ActSceneExit:
            return "シーンを終了";
        case StrId::ActSceneDelete:
            return "シーンを削除";
        case StrId::ActSceneEval:
            return "コードを実行";
        case StrId::ActSceneLogs:
            return "ログを確認";
        case StrId::ActSceneDataRead:
            return "シーンのデータを読む";
        case StrId::ActSceneDataWrite:
            return "シーンのデータを書く";
        case StrId::ActSceneFilesList:
            return "保存ファイルを確認";
        case StrId::ActSceneFilesRead:
            return "保存ファイルを読む";
        case StrId::ActSceneFilesRemove:
            return "保存ファイルを削除";
        case StrId::ActMemoryAdd:
            return "記憶を追加";
        case StrId::ActMemoryList:
            return "記憶を確認";
        case StrId::ActMemoryUpdate:
            return "記憶を更新";
        case StrId::ActMemoryRemove:
            return "記憶を削除";
        case StrId::ActTodoAdd:
            return "ToDo を追加";
        case StrId::ActTodoList:
            return "ToDo を確認";
        case StrId::ActTodoUpdate:
            return "ToDo を更新";
        case StrId::ActTodoDone:
            return "ToDo を完了";
        case StrId::ActTodoRemove:
            return "ToDo を削除";
        case StrId::ActSettingsGet:
            return "設定を確認";
        case StrId::ActSettingsSet:
            return "設定を変更";
        case StrId::ActDeviceStatus:
            return "端末の状態を確認";
        case StrId::ActNotifyList:
            return "通知を確認";
        case StrId::ActNotifyDismiss:
            return "通知を消す";
        case StrId::ActDocsRead:
            return "ドキュメントを読む";
        case StrId::ActHttpFetch:
            return "ウェブページを取得";
        case StrId::ActLuaRun:
            return "Lua スクリプトを実行";
        case StrId::ActToolsFind:
            return "シーンツールを検索";
        case StrId::ActToolsCall:
            return "シーンツールを呼び出す";
        case StrId::ActUiTree:
            return "画面を確認";
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
