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
        case StrId::Battery:
            return "バッテリー";
        case StrId::Time:
            return "時刻";
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
        case StrId::TouchTestArea:
            return "タッチテスト";
        case StrId::TestUntested:
            return "未テスト";
        case StrId::TestPassed:
            return "合格";
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
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
