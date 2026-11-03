// Japanese (ja) device UI text (spec 8.3). Kanji render with the built-in
// font's Simplified Chinese glyph shapes until a Japanese-shaped font can
// be loaded per language (spec 8.3).

#include "strings_tables.hpp"

namespace wisp::ui::detail {

const char* strings_ja(StrId id) {
    switch (id) {
        case StrId::BootTime:
            return "起動";
        case StrId::DeviceList:
            return "デバイス一覧";
        case StrId::StateOk:
            return "正常";
        case StrId::StateDegraded:
            return "機能制限";
        case StrId::StateMissing:
            return "未検出";
        case StrId::StateFailed:
            return "エラー";
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
        case StrId::Count:
            break;
    }
    return nullptr;
}

}  // namespace wisp::ui::detail
