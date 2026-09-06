#pragma once

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/key.h>

namespace wetype_config {
using namespace fcitx;

enum class InputMode { Pinyin, DoublePinyin, Wubi };
FCITX_CONFIG_ENUM_NAME(InputMode, "拼音输入", "双拼输入", "五笔输入")
enum class DoublePinyinScheme { Ziranma, Sogou, Microsoft, Xiaohe, PinyinJiajia, Ziguang, SmartABC };
FCITX_CONFIG_ENUM_NAME(DoublePinyinScheme, "自然码", "搜狗", "微软", "小鹤",
                       "拼音加加", "紫光", "智能 ABC")
enum class WubiScheme { Wubi86, Wubi98, NewCentury };
FCITX_CONFIG_ENUM_NAME(WubiScheme, "86 五笔", "98 五笔", "新世纪五笔")
enum class DefaultLanguage { Chinese, English };
FCITX_CONFIG_ENUM_NAME(DefaultLanguage, "中文", "英文")
enum class ThemeMode { System, Light, Dark };
FCITX_CONFIG_ENUM_NAME(ThemeMode, "跟随系统", "浅色", "深色")

FCITX_CONFIGURATION(
    InputConfig,
    Option<InputMode> mode{this, "Mode", "输入方式", InputMode::Pinyin};
    Option<DoublePinyinScheme> doublePinyin{this, "DoublePinyin", "双拼方案",
                                            DoublePinyinScheme::Ziranma};
    Option<WubiScheme> wubi{this, "Wubi", "五笔方案", WubiScheme::Wubi86};
    Option<bool> smartInput{this, "SmartInput", "智能拼写", true};
    Option<bool> emojiRecommend{this, "EmojiRecommend", "表情和颜文字推荐", true};
    Option<bool> slashPunctuation{this, "SlashPunctuation", "输入中文时将 /? 替换为 、", true};
    Option<bool> symbolAutoChange{this, "SymbolAutoChange", "符号自动转换", true};
    Option<bool> symbolAutoPair{this, "SymbolAutoPair", "符号自动补全", true};
    Option<DefaultLanguage> defaultLanguage{this, "DefaultLanguage", "默认输入语言",
                                            DefaultLanguage::Chinese};
    Option<bool> fuzzyNl{this, "FuzzyNl", "模糊拼音 n/l", false};
    Option<bool> fuzzyRl{this, "FuzzyRl", "模糊拼音 r/l", false};
    Option<bool> fuzzyHf{this, "FuzzyHf", "模糊拼音 h/f", false};
    Option<bool> fuzzyGk{this, "FuzzyGk", "模糊拼音 g/k", false};
    Option<bool> fuzzyCCh{this, "FuzzyCCh", "模糊拼音 c/ch", false};
    Option<bool> fuzzySSh{this, "FuzzySSh", "模糊拼音 s/sh", false};
    Option<bool> fuzzyZZh{this, "FuzzyZZh", "模糊拼音 z/zh", false};
    Option<bool> standalone{this, "Standalone", "单机模式", false};)

FCITX_CONFIGURATION(
    VoiceConfig,
    Option<bool> launchShortcut{this, "LaunchShortcut", "启动语音输入快捷键", true};
    KeyListOption launchKey{
        this, "LaunchKey", "启动语音输入按键",
        {Key("Control+Super+Shift_L")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess,
                          KeyConstrainFlag::AllowModifierOnly})};
    Option<bool> holdShortcut{this, "HoldShortcut", "按住说话快捷键", true};
    KeyListOption holdKey{
        this, "HoldKey", "按住说话按键", {Key("Control+Super_L")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess,
                          KeyConstrainFlag::AllowModifierOnly})};
    Option<std::string> microphone{this, "Microphone", "麦克风", "自动检测"};
    Option<std::string> punctuation{this, "Punctuation", "标点设置", "智能标点"};
    Option<bool> smartPolish{this, "SmartPolish", "语音智能整理", true};)

FCITX_CONFIGURATION(
    PhraseClipboardConfig,
    Option<bool> clipboard{this, "Clipboard", "在输入法剪贴板中展示复制内容", false};)

FCITX_CONFIGURATION(
    AppearanceConfig,
    Option<int, IntConstrain> candidateSize{this, "CandidateSize", "候选字大小", 13,
                                            IntConstrain(10, 18)};
    Option<int, IntConstrain> pageSize{this, "PageSize", "每页候选数", 5,
                                       IntConstrain(3, 9)};
    Option<bool> vertical{this, "Vertical", "候选词竖排", false};
    Option<ThemeMode> theme{this, "Theme", "主题模式", ThemeMode::System};)

FCITX_CONFIGURATION(
    ShortcutConfig,
    Option<bool> shiftSwitch{this, "ShiftSwitch", "使用 Shift 切换中英文", true};
    Option<bool> ctrlSwitch{this, "CtrlSwitch", "使用 Ctrl 切换中英文", false};
    KeyListOption languageSwitchKeys{
        this, "LanguageSwitchKeys", "中英文切换按键", {Key("Shift_L")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess,
                          KeyConstrainFlag::AllowModifierOnly})};
    Option<bool> aiAssistant{this, "AiAssistant", "AI 助手（=）", true};
    KeyListOption aiAssistantKeys{
        this, "AiAssistantKeys", "AI 助手按键", {Key("equal")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess})};
    Option<bool> vMode{this, "VMode", "V 模式", true};
    KeyListOption vModeKeys{
        this, "VModeKeys", "V 模式按键", {Key("v")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess})};
    Option<bool> halfFull{this, "HalfFull", "全半角输入切换", false};
    KeyListOption halfFullKeys{
        this, "HalfFullKeys", "全半角输入切换按键", {Key("Shift+space")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess})};
    Option<bool> punctuationSwitch{this, "PunctuationSwitch", "中文下中英标点切换", true};
    KeyListOption punctuationSwitchKeys{
        this, "PunctuationSwitchKeys", "中英标点切换按键",
        {Key("Control+period")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess})};
    Option<bool> traditionalSwitch{this, "TraditionalSwitch", "简繁体输入切换", false};
    KeyListOption traditionalSwitchKeys{
        this, "TraditionalSwitchKeys", "简繁体输入切换按键",
        {Key("Control+Shift+f")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess})};
    Option<bool> pageMinusEqual{this, "PageMinusEqual", "减号等号翻页", true};
    Option<bool> pageBrackets{this, "PageBrackets", "左右中括号翻页", true};
    Option<bool> pageCommaPeriod{this, "PageCommaPeriod", "逗号句号翻页", false};
    Option<bool> pageShiftTab{this, "PageShiftTab", "Shift+Tab / Tab 翻页", false};
    KeyListOption previousPageKeys{
        this, "PreviousPageKeys", "额外向上翻页按键", {},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess})};
    KeyListOption nextPageKeys{
        this, "NextPageKeys", "额外向下翻页按键", {},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess})};
    Option<bool> selectSemicolonQuote{this, "SelectSemicolonQuote", "分号、引号选择第 2、3 位", false};
    Option<bool> selectCtrl{this, "SelectCtrl", "左右 Ctrl 选择第 2、3 位", false};
    KeyListOption secondCandidateKeys{
        this, "SecondCandidateKeys", "选择第 2 位候选词按键", {},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess,
                          KeyConstrainFlag::AllowModifierOnly})};
    KeyListOption thirdCandidateKeys{
        this, "ThirdCandidateKeys", "选择第 3 位候选词按键", {},
        KeyListConstrain({KeyConstrainFlag::AllowModifierLess,
                          KeyConstrainFlag::AllowModifierOnly})};)

FCITX_CONFIGURATION(
    DeviceConfig,
    Option<bool> clipboardSync{this, "ClipboardSync", "跨设备复制粘贴", false};
    Option<bool> dictionarySync{this, "DictionarySync", "个人词库同步", false};
    Option<bool> phraseSync{this, "PhraseSync", "常用语同步", false};)

FCITX_CONFIGURATION(
    UpdateConfig,
    Option<bool> autoUpdate{this, "AutoUpdate", "有新版本时自动更新", true};)

FCITX_CONFIGURATION(
    WeTypeConfig,
    Option<InputConfig> input{this, "Input", "输入"};
    Option<VoiceConfig> voice{this, "Voice", "语音输入"};
    Option<PhraseClipboardConfig> phrases{this, "PhrasesClipboard", "常用语和剪贴板"};
    Option<AppearanceConfig> appearance{this, "Appearance", "外观"};
    Option<ShortcutConfig> shortcuts{this, "Shortcuts", "快捷键"};
    Option<DeviceConfig> devices{this, "Devices", "跨设备"};
    Option<UpdateConfig> update{this, "Update", "升级和反馈"};)

} // namespace wetype_config
