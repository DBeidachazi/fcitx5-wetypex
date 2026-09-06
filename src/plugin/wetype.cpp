#include "../common/json.hpp"
#include "config.hpp"
#include <cerrno>
#include <clipboard_public.h>
#include <csignal>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterfacemanager.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <libime/pinyin/pinyinencoder.h>
#include <map>
#include <queue>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
extern char **environ;
namespace {
using namespace fcitx;
static std::string formatPinyinPreedit(const std::string &raw) {
  auto begin = std::find_if(raw.begin(), raw.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || c == '\'';
  });
  if (begin == raw.end())
    return raw;
  const size_t prefixSize = begin - raw.begin();
  std::string input(begin, raw.end());
  if (input.size() < 2 || input.find('\'') != std::string::npos ||
      !std::all_of(input.begin(), input.end(),
                   [](unsigned char c) { return c >= 'a' && c <= 'z'; }))
    return raw;
  auto graph = libime::PinyinEncoder::parseUserPinyin(
      input, libime::PinyinFuzzyFlag::None);
  using Node = libime::SegmentGraphNode;
  std::queue<const Node *> pending;
  std::map<const Node *, const Node *> previous;
  pending.push(&graph.start());
  previous[&graph.start()] = nullptr;
  while (!pending.empty() && !previous.count(&graph.end())) {
    auto *node = pending.front();
    pending.pop();
    for (const auto &next : node->nexts())
      if (!previous.count(&next)) {
        previous[&next] = node;
        pending.push(&next);
      }
  }
  if (!previous.count(&graph.end()))
    return raw;
  std::vector<size_t> boundaries;
  for (auto *node = &graph.end(); node != &graph.start();
       node = previous[node])
    boundaries.push_back(node->index());
  std::reverse(boundaries.begin(), boundaries.end());
  if (boundaries.size() < 2)
    return raw;
  std::vector<size_t> displayBoundaries;
  size_t start = 0;
  for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
    auto segment = std::string_view(input).substr(start, boundaries[i] - start);
    if (segment.size() == 1 && segment != "a" && segment != "e" &&
        segment != "o")
      break; // Keep the unfinished suffix together, e.g. zhong'we.
    displayBoundaries.push_back(boundaries[i]);
    start = boundaries[i];
  }
  if (displayBoundaries.empty())
    return raw; // Preserve smart-spelling input instead of inventing breaks.
  displayBoundaries.push_back(input.size());
  std::string result = raw.substr(0, prefixSize);
  start = 0;
  for (size_t i = 0; i < displayBoundaries.size(); ++i) {
    if (i)
      result += '\'';
    result.append(input, start, displayBoundaries[i] - start);
    start = displayBoundaries[i];
  }
  return result;
}
class WeType;
struct State : InputContextProperty {
  uint64_t id, epoch = 1, seq = 0, applied = 0, revision = 0;
  uint64_t cloudPollUntil = 0, lastCloudPoll = 0;
  std::string preedit;
  bool english = false, fullWidth = false, traditional = false,
       englishPunctuation = false;
  KeySym modifierCandidate = FcitxKey_None;
  TrackableObjectReference<InputContext> ic;
  State(uint64_t n, InputContext &context) : id(n), ic(context.watch()) {}
};
class Word : public CandidateWord {
  WeType *engine_;
  unsigned index_;
  uint64_t revision_;

public:
  Word(WeType *e, std::string text, unsigned index, uint64_t revision)
      : CandidateWord(Text(text)), engine_(e), index_(index),
        revision_(revision) {}
  void select(InputContext *ic) const override;
};
class WeType : public InputMethodEngine {
  Instance *instance_;
  uint64_t nextId_ = 0;
  FactoryFor<State> factory_;
  SimpleAction settingsAction_;
  Connection settingsConnection_;
  std::map<uint64_t, TrackableObjectReference<InputContext>> contexts_;
  pid_t child_ = -1;
  int read_ = -1, write_ = -1;
  std::unique_ptr<EventSourceIO> reader_, writer_;
  std::unique_ptr<EventSourceTime> timer_;
  bool ready_ = false, failed_ = false;
  uint64_t activity_ = 0;
  uint64_t restartAt_ = 0, restartBackoff_ = 250000;
  bool restartNeedsOpen_ = false;
  uint64_t syncTick_ = 0, lastInboxVersion_ = 0;
  uint64_t lastVoiceVersion_ = 0;
  std::string inbound_, outbound_;
  std::string lastLocalClipboard_, lastRemoteClipboard_;
  bool voiceRecording_ = false, voiceHold_ = false;
  pid_t syncChild_ = -1;
  void startSync() {
    if (syncChild_ > 0 && waitpid(syncChild_, nullptr, WNOHANG) == 0)
      return;
    syncChild_ = -1;
    const char *override = getenv("WETYPE_SYNCD_LAUNCHER");
    std::string launcher = override && *override ? override : WETYPE_SYNCD;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    char *argv[] = {launcher.data(), nullptr};
    if (posix_spawn(&syncChild_, launcher.c_str(), &actions, nullptr, argv,
                    environ))
      syncChild_ = -1;
    posix_spawn_file_actions_destroy(&actions);
  }
  int pageSize_ = 5;
  int keyboard_ = 0;
  bool vertical_ = false, chinesePunctuation_ = true, slashPunctuation_ = true,
       symbolAutoChange_ = true, clipboardEnabled_ = false,
       networkEnabled_ = true;
  wetype_config::WeTypeConfig config_;
  std::filesystem::path stateDirectory() const {
    const char *state = getenv("WETYPE_STATE_DIR"),
               *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (state)
      return state;
    auto directory = data ? std::filesystem::path(data)
                          : std::filesystem::path(home ? home : "") /
                                ".local/share";
    return directory / "fcitx5-wetypex/state";
  }
  void reloadSettings() {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    std::string path =
        config ? config : std::string(home ? home : "") + "/.config";
    const auto newPath = path + "/fcitx5/wetypex.json";
    std::ifstream f(newPath);
    std::string bytes(16385, '\0');
    f.read(bytes.data(), bytes.size());
    bytes.resize(f.gcount());
    auto j = bytes.size() > 16384 ? wire::Json{} : wire::parse(bytes);
    pageSize_ = std::clamp(int(wire::number(j.get(), "page_size", 5)), 3, 9);
    keyboard_ = wire::number(j.get(), "keyboard", 0) == 5 ? 5 : 0;
    vertical_ = json_object_get_boolean(wire::get(j.get(), "vertical"));
    auto *punctuation = wire::get(j.get(), "chinese_punctuation");
    chinesePunctuation_ = !punctuation || json_object_get_boolean(punctuation);
    auto *slash = wire::get(j.get(), "slash_punctuation");
    slashPunctuation_ = !slash || json_object_get_boolean(slash);
    auto *symbolChange = wire::get(j.get(), "symbol_auto_change");
    symbolAutoChange_ =
        !symbolChange || json_object_get_boolean(symbolChange);
    clipboardEnabled_ =
        json_object_get_boolean(wire::get(j.get(), "clipboard_enabled"));
    networkEnabled_ =
        !json_object_get_boolean(wire::get(j.get(), "standalone"));
    auto *input = config_.input.mutableValue();
    auto mode = wire::str(j.get(), "input_mode");
    input->mode.setValue(mode == "wubi" ? wetype_config::InputMode::Wubi
                         : mode == "double_pinyin"
                             ? wetype_config::InputMode::DoublePinyin
                             : wetype_config::InputMode::Pinyin);
    input->wubi.setValue(static_cast<wetype_config::WubiScheme>(std::clamp(
        int(wire::number(j.get(), "wubi_solution", 0)), 0, 2)));
    input->doublePinyin.setValue(
        static_cast<wetype_config::DoublePinyinScheme>(std::clamp(
            int(wire::number(j.get(), "double_pinyin_scheme", 0)), 0, 6)));
    input->smartInput.setValue(!wire::get(j.get(), "smart_input") ||
                               json_object_get_boolean(
                                   wire::get(j.get(), "smart_input")));
    input->emojiRecommend.setValue(
        !wire::get(j.get(), "emoji_recommend") ||
        json_object_get_boolean(wire::get(j.get(), "emoji_recommend")));
    input->slashPunctuation.setValue(slashPunctuation_);
    input->symbolAutoChange.setValue(
        !wire::get(j.get(), "symbol_auto_change") ||
        json_object_get_boolean(wire::get(j.get(), "symbol_auto_change")));
    input->symbolAutoPair.setValue(
        !wire::get(j.get(), "symbol_auto_pair") ||
        json_object_get_boolean(wire::get(j.get(), "symbol_auto_pair")));
    input->standalone.setValue(!networkEnabled_);
    input->defaultLanguage.setValue(
        wire::str(j.get(), "default_language") == "english"
            ? wetype_config::DefaultLanguage::English
            : wetype_config::DefaultLanguage::Chinese);
    input->fuzzyNl.setValue(json_object_get_boolean(wire::get(j.get(), "fuzzy_nl")));
    input->fuzzyRl.setValue(json_object_get_boolean(wire::get(j.get(), "fuzzy_rl")));
    input->fuzzyHf.setValue(json_object_get_boolean(wire::get(j.get(), "fuzzy_hf")));
    input->fuzzyGk.setValue(json_object_get_boolean(wire::get(j.get(), "fuzzy_gk")));
    input->fuzzyCCh.setValue(json_object_get_boolean(wire::get(j.get(), "fuzzy_c_ch")));
    input->fuzzySSh.setValue(json_object_get_boolean(wire::get(j.get(), "fuzzy_s_sh")));
    input->fuzzyZZh.setValue(json_object_get_boolean(wire::get(j.get(), "fuzzy_z_zh")));
    auto *phrases = config_.phrases.mutableValue();
    phrases->clipboard.setValue(clipboardEnabled_);
    auto *appearance = config_.appearance.mutableValue();
    appearance->pageSize.setValue(pageSize_);
    appearance->candidateSize.setValue(std::clamp(
        int(wire::number(j.get(), "candidate_size", 13)), 10, 18));
    appearance->vertical.setValue(vertical_);
    appearance->theme.setValue(static_cast<wetype_config::ThemeMode>(
        std::clamp(int(wire::number(j.get(), "theme_mode", 0)), 0, 2)));
    auto *devices = config_.devices.mutableValue();
    devices->clipboardSync.setValue(
        json_object_get_boolean(wire::get(j.get(), "device_clipboard_sync")));
    devices->dictionarySync.setValue(
        json_object_get_boolean(wire::get(j.get(), "device_dictionary_sync")));
    devices->phraseSync.setValue(
        json_object_get_boolean(wire::get(j.get(), "device_phrase_sync")));
    auto boolSetting = [&](const char *name, bool fallback) {
      auto *value = wire::get(j.get(), name);
      return value ? bool(json_object_get_boolean(value)) : fallback;
    };
    auto *voice = config_.voice.mutableValue();
    voice->launchShortcut.setValue(boolSetting("voice_launch_shortcut", true));
    voice->holdShortcut.setValue(boolSetting("voice_hold_shortcut", true));
    voice->smartPolish.setValue(boolSetting("voice_smart_polish", true));
    auto stringSetting = [&](const char *name, const char *fallback) {
      auto value = wire::str(j.get(), name);
      return value.empty() ? std::string(fallback) : value;
    };
    voice->launchKey.setValue(Key::keyListFromString(
        stringSetting("voice_launch_key", "Control+Super+Shift_L")));
    voice->holdKey.setValue(Key::keyListFromString(
        stringSetting("voice_hold_key", "Control+Super_L")));
    voice->microphone.setValue(
        stringSetting("voice_microphone", "自动检测"));
    voice->punctuation.setValue(
        stringSetting("voice_punctuation", "智能标点"));
    auto *shortcuts = config_.shortcuts.mutableValue();
    shortcuts->shiftSwitch.setValue(boolSetting("shift_switch", true));
    shortcuts->ctrlSwitch.setValue(boolSetting("ctrl_switch", false));
    shortcuts->aiAssistant.setValue(boolSetting("ai_assistant", true));
    shortcuts->vMode.setValue(boolSetting("v_mode", true));
    shortcuts->halfFull.setValue(boolSetting("half_full_switch", false));
    shortcuts->punctuationSwitch.setValue(boolSetting("punctuation_switch", true));
    shortcuts->traditionalSwitch.setValue(boolSetting("traditional_switch", false));
    shortcuts->pageMinusEqual.setValue(boolSetting("page_minus_equal", true));
    shortcuts->pageBrackets.setValue(boolSetting("page_brackets", true));
    shortcuts->pageCommaPeriod.setValue(boolSetting("page_comma_period", false));
    shortcuts->pageShiftTab.setValue(boolSetting("page_shift_tab", false));
    shortcuts->selectSemicolonQuote.setValue(boolSetting("select_semicolon_quote", false));
    shortcuts->selectCtrl.setValue(boolSetting("select_ctrl", false));
    auto loadKeys = [&](auto &option, const char *name) {
      auto value = wire::str(j.get(), name);
      if (!value.empty())
        option.setValue(Key::keyListFromString(value));
    };
    loadKeys(shortcuts->languageSwitchKeys, "language_switch_keys");
    loadKeys(shortcuts->aiAssistantKeys, "ai_assistant_keys");
    loadKeys(shortcuts->vModeKeys, "v_mode_keys");
    loadKeys(shortcuts->halfFullKeys, "half_full_keys");
    loadKeys(shortcuts->punctuationSwitchKeys, "punctuation_switch_keys");
    loadKeys(shortcuts->traditionalSwitchKeys, "traditional_switch_keys");
    loadKeys(shortcuts->previousPageKeys, "previous_page_keys");
    loadKeys(shortcuts->nextPageKeys, "next_page_keys");
    loadKeys(shortcuts->secondCandidateKeys, "second_candidate_keys");
    loadKeys(shortcuts->thirdCandidateKeys, "third_candidate_keys");
    config_.update.mutableValue()->autoUpdate.setValue(
        boolSetting("auto_update", true));
  }
  void saveNativeConfig() {
    const char *configHome = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    std::filesystem::path directory =
        configHome ? configHome
                   : std::filesystem::path(home ? home : "") / ".config";
    directory /= "fcitx5";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
      return;
    auto path = directory / "wetypex.json";
    std::ifstream oldFile(path);
    std::string oldBytes((std::istreambuf_iterator<char>(oldFile)), {});
    auto json = wire::parse(oldBytes);
    if (!json || !json_object_is_type(json.get(), json_type_object))
      json = wire::object();
    const auto &input = *config_.input;
    auto mode = *input.mode == wetype_config::InputMode::Wubi
                    ? "wubi"
                : *input.mode == wetype_config::InputMode::DoublePinyin
                    ? "double_pinyin"
                    : "pinyin";
    wire::put(json.get(), "input_mode", std::string(mode));
    wire::put(json.get(), "keyboard",
              int64_t(*input.mode == wetype_config::InputMode::Wubi ? 5 : 0));
    wire::put(json.get(), "wubi_solution", int64_t(*input.wubi));
    wire::put(json.get(), "double_pinyin_scheme",
              int64_t(*input.doublePinyin));
    wire::put(json.get(), "smart_input", bool(*input.smartInput));
    wire::put(json.get(), "emoji_recommend", bool(*input.emojiRecommend));
    wire::put(json.get(), "slash_punctuation", bool(*input.slashPunctuation));
    wire::put(json.get(), "symbol_auto_change", bool(*input.symbolAutoChange));
    wire::put(json.get(), "symbol_auto_pair", bool(*input.symbolAutoPair));
    wire::put(json.get(), "standalone", bool(*input.standalone));
    wire::put(json.get(), "default_language",
              std::string(*input.defaultLanguage ==
                                  wetype_config::DefaultLanguage::English
                              ? "english"
                              : "chinese"));
    wire::put(json.get(), "fuzzy_nl", bool(*input.fuzzyNl));
    wire::put(json.get(), "fuzzy_rl", bool(*input.fuzzyRl));
    wire::put(json.get(), "fuzzy_hf", bool(*input.fuzzyHf));
    wire::put(json.get(), "fuzzy_gk", bool(*input.fuzzyGk));
    wire::put(json.get(), "fuzzy_c_ch", bool(*input.fuzzyCCh));
    wire::put(json.get(), "fuzzy_s_sh", bool(*input.fuzzySSh));
    wire::put(json.get(), "fuzzy_z_zh", bool(*input.fuzzyZZh));
    const auto &appearance = *config_.appearance;
    wire::put(json.get(), "page_size", int64_t(*appearance.pageSize));
    wire::put(json.get(), "candidate_size",
              int64_t(*appearance.candidateSize));
    wire::put(json.get(), "vertical", bool(*appearance.vertical));
    wire::put(json.get(), "theme_mode", int64_t(*appearance.theme));
    wire::put(json.get(), "clipboard_enabled",
              bool(config_.phrases->clipboard.value()));
    const auto &devices = *config_.devices;
    wire::put(json.get(), "device_clipboard_sync",
              bool(*devices.clipboardSync));
    wire::put(json.get(), "device_dictionary_sync",
              bool(*devices.dictionarySync));
    wire::put(json.get(), "device_phrase_sync", bool(*devices.phraseSync));
    const auto &voice = *config_.voice;
    wire::put(json.get(), "voice_launch_shortcut", bool(*voice.launchShortcut));
    wire::put(json.get(), "voice_hold_shortcut", bool(*voice.holdShortcut));
    wire::put(json.get(), "voice_smart_polish", bool(*voice.smartPolish));
    wire::put(json.get(), "voice_launch_key",
              Key::keyListToString(*voice.launchKey));
    wire::put(json.get(), "voice_hold_key",
              Key::keyListToString(*voice.holdKey));
    wire::put(json.get(), "voice_microphone", *voice.microphone);
    wire::put(json.get(), "voice_punctuation", *voice.punctuation);
    const auto &shortcuts = *config_.shortcuts;
    wire::put(json.get(), "shift_switch", bool(*shortcuts.shiftSwitch));
    wire::put(json.get(), "ctrl_switch", bool(*shortcuts.ctrlSwitch));
    wire::put(json.get(), "ai_assistant", bool(*shortcuts.aiAssistant));
    wire::put(json.get(), "v_mode", bool(*shortcuts.vMode));
    wire::put(json.get(), "half_full_switch", bool(*shortcuts.halfFull));
    wire::put(json.get(), "punctuation_switch", bool(*shortcuts.punctuationSwitch));
    wire::put(json.get(), "traditional_switch", bool(*shortcuts.traditionalSwitch));
    wire::put(json.get(), "page_minus_equal", bool(*shortcuts.pageMinusEqual));
    wire::put(json.get(), "page_brackets", bool(*shortcuts.pageBrackets));
    wire::put(json.get(), "page_comma_period", bool(*shortcuts.pageCommaPeriod));
    wire::put(json.get(), "page_shift_tab", bool(*shortcuts.pageShiftTab));
    wire::put(json.get(), "select_semicolon_quote", bool(*shortcuts.selectSemicolonQuote));
    wire::put(json.get(), "select_ctrl", bool(*shortcuts.selectCtrl));
    auto saveKeys = [&](const char *name, const auto &option) {
      wire::put(json.get(), name, Key::keyListToString(*option));
    };
    saveKeys("language_switch_keys", shortcuts.languageSwitchKeys);
    saveKeys("ai_assistant_keys", shortcuts.aiAssistantKeys);
    saveKeys("v_mode_keys", shortcuts.vModeKeys);
    saveKeys("half_full_keys", shortcuts.halfFullKeys);
    saveKeys("punctuation_switch_keys", shortcuts.punctuationSwitchKeys);
    saveKeys("traditional_switch_keys", shortcuts.traditionalSwitchKeys);
    saveKeys("previous_page_keys", shortcuts.previousPageKeys);
    saveKeys("next_page_keys", shortcuts.nextPageKeys);
    saveKeys("second_candidate_keys", shortcuts.secondCandidateKeys);
    saveKeys("third_candidate_keys", shortcuts.thirdCandidateKeys);
    wire::put(json.get(), "auto_update", bool(*config_.update->autoUpdate));
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << wire::dump(json.get()) << '\n';
    output.close();
    chmod(temporary.c_str(), 0600);
    std::filesystem::rename(temporary, path, error);
    reloadSettings();
  }
  void applyAppearance() {
    const char *configHome = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    std::filesystem::path path =
        configHome ? configHome
                   : std::filesystem::path(home ? home : "") / ".config";
    path /= "fcitx5/conf/classicui.conf";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
      return;
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
      lines.push_back(line);
    const auto &appearance = *config_.appearance;
    std::map<std::string, std::string> values = {
        {"Theme", *appearance.theme == wetype_config::ThemeMode::Dark
                      ? "wetypex-dark"
                      : "wetypex-light"},
        {"DarkTheme", "wetypex-dark"},
        {"UseDarkTheme", *appearance.theme == wetype_config::ThemeMode::System
                             ? "True"
                             : "False"},
        {"UseAccentColor", "False"},
        {"PreferTextIcon", "False"},
        {"Font", "Noto Sans CJK SC " +
                     std::to_string(*appearance.candidateSize)}};
    for (auto &[key, value] : values) {
      bool found = false;
      for (auto &existing : lines)
        if (existing.starts_with(key + "=")) {
          existing = key + "=" + value;
          found = true;
          break;
        }
      if (!found)
        lines.push_back(key + "=" + value);
    }
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    for (auto &entry : lines)
      output << entry << '\n';
    output.close();
    chmod(temporary.c_str(), 0600);
    std::filesystem::rename(temporary, path, error);
    if (!error)
      instance_->reloadAddonConfig("classicui");
  }
  void applyDeviceFunctions() {
    std::ifstream input(stateDirectory() / "sync-state.json");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    auto state = wire::parse(bytes);
    auto group = wire::number(state.get(), "group_id");
    if (group <= 0)
      return;
    const auto &devices = *config_.devices;
    int mask = (*devices.clipboardSync ? 1 : 0) |
               (*devices.phraseSync ? 2 : 0) |
               (*devices.dictionarySync ? 4 : 0);
    startProcess({WETYPE_ACCOUNT_TOOL, "set-functions", std::to_string(group),
                  std::to_string(mask)});
  }
  void recordClipboard(InputContext *ic) {
    auto *addon = instance_->addonManager().addon("clipboard", true);
    if (!addon)
      return;
    auto value = addon->call<IClipboard::clipboard>(ic);
    if (value.empty() || value.size() > 16384)
      return;
    auto directory = stateDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
      return;
    chmod(directory.c_str(), 0700);
    if (clipboardEnabled_) {
      auto path = directory / "clipboard-history.json";
      std::vector<std::string> entries{value};
      std::ifstream input(path);
      std::string bytes((std::istreambuf_iterator<char>(input)), {});
      if (bytes.size() <= 1048576) {
        auto old = wire::parse(bytes);
        if (old && json_object_is_type(old.get(), json_type_array))
          for (size_t i = 0;
               i < json_object_array_length(old.get()) && entries.size() < 20;
               ++i) {
            auto *item = json_object_array_get_idx(old.get(), i);
            if (!json_object_is_type(item, json_type_string))
              continue;
            std::string text = json_object_get_string(item);
            if (text.size() <= 16384 &&
                std::find(entries.begin(), entries.end(), text) == entries.end())
              entries.push_back(std::move(text));
          }
      }
      auto array = wire::Json(json_object_new_array());
      for (const auto &entry : entries)
        json_object_array_add(
            array.get(), json_object_new_string_len(entry.data(), entry.size()));
      auto temporary = path;
      temporary += ".tmp";
      std::ofstream output(temporary, std::ios::trunc);
      output << wire::dump(array.get()) << '\n';
      output.close();
      chmod(temporary.c_str(), 0600);
      std::filesystem::rename(temporary, path, error);
    }
    if (!networkEnabled_ || value == lastLocalClipboard_ ||
        value == lastRemoteClipboard_)
      return;
    lastLocalClipboard_ = value;
    auto outbox = wire::object();
    wire::put(outbox.get(), "id",
              std::to_string(now(CLOCK_REALTIME)) + "-" +
                  std::to_string(getpid()));
    wire::put(outbox.get(), "text", value);
    auto path = directory / "clipboard-outbox.json";
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream outgoing(temporary, std::ios::trunc);
    outgoing << wire::dump(outbox.get()) << '\n';
    outgoing.close();
    chmod(temporary.c_str(), 0600);
    std::filesystem::rename(temporary, path, error);
  }
  void receiveRemoteClipboard() {
    auto path = stateDirectory() / "clipboard-inbox.json";
    std::ifstream input(path);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || bytes.size() > 65536)
      return;
    auto message = wire::parse(bytes);
    auto version = uint64_t(wire::number(message.get(), "version"));
    auto text = wire::str(message.get(), "text");
    if (!version || version <= lastInboxVersion_ || text.empty() ||
        text.size() > 16384)
      return;
    auto *addon = instance_->addonManager().addon("clipboard", true);
    if (!addon)
      return;
    addon->call<IClipboard::setClipboard>("WeTypeX 跨设备", text);
    lastInboxVersion_ = version;
    lastRemoteClipboard_ = text;
    lastLocalClipboard_ = text;
  }
  void startVoice(InputContext *ic, bool hold) {
    if (voiceRecording_)
      return;
    auto *s = state(ic);
    if (!s->preedit.empty()) {
      send(ic, "reset");
      s->preedit.clear();
      ic->inputPanel().reset();
      panel(ic, s);
    }
    voiceRecording_ = true;
    voiceHold_ = hold;
    startProcess({WETYPE_VOICE, "start"});
  }
  void stopVoice() {
    if (!voiceRecording_)
      return;
    voiceRecording_ = false;
    voiceHold_ = false;
    startProcess({WETYPE_VOICE, "stop"});
  }
  void receiveVoice() {
    std::ifstream input(stateDirectory() / "voice-inbox.json");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || bytes.size() > 65536)
      return;
    auto message = wire::parse(bytes);
    auto version = uint64_t(wire::number(message.get(), "version"));
    auto text = wire::str(message.get(), "text");
    if (!version || version <= lastVoiceVersion_ || text.empty())
      return;
    lastVoiceVersion_ = version;
    for (auto &[id, ref] : contexts_)
      if (auto *ic = ref.get(); ic && ic->hasFocus()) {
        ic->commitString(text);
        break;
      }
  }
  State *state(InputContext *ic) {
    auto *s = ic->propertyFor(&factory_);
    contexts_[s->id] = ic->watch();
    return s;
  }
  void panel(InputContext *ic, State *s) {
    ic->inputPanel().setAuxUp(Text());
    const std::string displayed =
        *config_.input->mode == wetype_config::InputMode::Pinyin
            ? formatPinyinPreedit(s->preedit)
            : s->preedit;
    Text text(displayed);
    text.setCursor(displayed.size());
    ic->inputPanel().setClientPreedit(text);
    // Windows 2.1.3.18 shows composition inline in the target application;
    // the candidate bar contains candidates only.
    ic->inputPanel().setPreedit(Text());
    ic->inputPanel().setAuxDown(
        Text(failed_   ? "WeTypeX 核心正在恢复…"
             : !ready_ ? "WeTypeX 核心启动中…"
                       : ""));
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  void stop() {
    reader_.reset();
    writer_.reset();
    if (read_ >= 0)
      close(read_);
    if (write_ >= 0)
      close(write_);
    read_ = write_ = -1;
    if (child_ > 0) {
      kill(child_, SIGKILL);
      while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
      }
      child_ = -1;
    }
    ready_ = false;
    restartAt_ = 0;
    outbound_.clear();
    inbound_.clear();
  }
  void scheduleRestart() {
    restartAt_ = now(CLOCK_MONOTONIC) + restartBackoff_;
    restartBackoff_ = std::min<uint64_t>(restartBackoff_ * 2, 30000000);
  }
  void fail() {
    stop();
    failed_ = true;
    restartNeedsOpen_ = true;
    scheduleRestart();
    for (auto &[id, ref] : contexts_)
      if (auto *ic = ref.get(); ic && ic->hasFocus()) {
        auto *s = state(ic);
        ++s->epoch;
        s->seq = s->applied = 0;
        s->preedit.clear();
        ic->inputPanel().setCandidateList(nullptr);
        panel(ic, s);
      }
  }
  bool start() {
    if (child_ > 0)
      return true;
    int to[2], from[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, to) < 0)
      return false;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, from) < 0) {
      close(to[0]);
      close(to[1]);
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, to[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, from[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, to[1]);
    posix_spawn_file_actions_addclose(&actions, from[0]);
    const char *override = getenv("WETYPE_BACKEND_LAUNCHER");
    std::string launcher = override ? override : WETYPE_LAUNCHER;
    char *argv[] = {launcher.data(), nullptr};
    int result = posix_spawn(&child_, launcher.c_str(), &actions, nullptr, argv,
                             environ);
    posix_spawn_file_actions_destroy(&actions);
    close(to[0]);
    close(from[1]);
    if (result) {
      close(to[1]);
      close(from[0]);
      child_ = -1;
      return false;
    }
    write_ = to[1];
    read_ = from[0];
    fcntl(write_, F_SETFL, fcntl(write_, F_GETFL) | O_NONBLOCK);
    fcntl(read_, F_SETFL, fcntl(read_, F_GETFL) | O_NONBLOCK);
    failed_ = false;
    restartAt_ = 0;
    activity_ = now(CLOCK_MONOTONIC);
    reader_ = instance_->eventLoop().addIOEvent(
        read_, IOEventFlag::In, [this](auto *, int fd, auto) {
          char buf[8192];
          for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
              inbound_.append(buf, n);
              if (inbound_.size() > 1048576) {
                fail();
                return true;
              }
            } else if (n == 0) {
              fail();
              return true;
            } else if (errno == EINTR)
              continue;
            else if (errno == EAGAIN)
              break;
            else {
              fail();
              return true;
            }
          }
          for (;;) {
            auto end = inbound_.find('\n');
            if (end == std::string::npos)
              break;
            auto line = inbound_.substr(0, end);
            inbound_.erase(0, end + 1);
            receive(line);
          }
          return true;
        });
    writer_ = instance_->eventLoop().addIOEvent(write_, IOEventFlag::Out,
                                                [this](auto *, int, auto) {
                                                  flush();
                                                  return true;
                                                });
    writer_->setEnabled(false);
    return true;
  }
  void flush() {
    while (!outbound_.empty() && write_ >= 0) {
      ssize_t n =
          ::send(write_, outbound_.data(), outbound_.size(), MSG_NOSIGNAL);
      if (n > 0)
        outbound_.erase(0, n);
      else if (n < 0 && errno == EINTR)
        continue;
      else if (n < 0 && errno == EAGAIN)
        break;
      else {
        fail();
        return;
      }
    }
    if (writer_)
      writer_->setEnabled(!outbound_.empty());
  }
  void send(InputContext *ic, const char *op, const std::string &key = "",
            int index = 0, int64_t revision = -1) {
    auto *s = state(ic);
    if (child_ <= 0 && !start()) {
      failed_ = true;
      scheduleRestart();
      panel(ic, s);
      return;
    }
    auto json = wire::object();
    wire::put(json.get(), "session", int64_t(s->id));
    wire::put(json.get(), "seq", int64_t(++s->seq));
    wire::put(json.get(), "epoch", int64_t(s->epoch));
    wire::put(json.get(), "op", std::string(op));
    wire::put(json.get(), "key", key);
    wire::put(json.get(), "index", int64_t(index));
    wire::put(json.get(), "revision", revision);
    wire::put(json.get(), "before", op == std::string("predict") ? key : "");
    wire::put(json.get(), "chinese_punctuation", int64_t(chinesePunctuation_));
    wire::put(json.get(), "keyboard", int64_t(keyboard_));
    const auto &inputConfig = *config_.input;
    static const int doubleSchemes[] = {5, 1, 2, 3, 4, 6, 7};
    wire::put(json.get(), "double_scheme",
              int64_t(*inputConfig.mode == wetype_config::InputMode::DoublePinyin
                          ? doubleSchemes[int(*inputConfig.doublePinyin)]
                          : 0));
    wire::put(json.get(), "smart_input", bool(*inputConfig.smartInput));
    wire::put(json.get(), "emoji_recommend",
              bool(*inputConfig.emojiRecommend));
    wire::put(json.get(), "fuzzy_nl", bool(*inputConfig.fuzzyNl));
    wire::put(json.get(), "fuzzy_rl", bool(*inputConfig.fuzzyRl));
    wire::put(json.get(), "fuzzy_hf", bool(*inputConfig.fuzzyHf));
    wire::put(json.get(), "fuzzy_gk", bool(*inputConfig.fuzzyGk));
    wire::put(json.get(), "fuzzy_c_ch", bool(*inputConfig.fuzzyCCh));
    wire::put(json.get(), "fuzzy_s_sh", bool(*inputConfig.fuzzySSh));
    wire::put(json.get(), "fuzzy_z_zh", bool(*inputConfig.fuzzyZZh));
    wire::put(json.get(), "traditional", int64_t(s->traditional));
    outbound_ += wire::dump(json.get()) + '\n';
    if (outbound_.size() > 262144) {
      fail();
      return;
    }
    activity_ = now(CLOCK_MONOTONIC);
    flush();
  }
  void receive(const std::string &line) {
    auto j = wire::parse(line);
    if (!j)
      return;
    if (wire::str(j.get(), "event") == "ready") {
      ready_ = true;
      failed_ = false;
      restartBackoff_ = 250000;
      activity_ = now(CLOCK_MONOTONIC);
      if (restartNeedsOpen_) {
        restartNeedsOpen_ = false;
        for (auto &[id, ref] : contexts_)
          if (auto *ic = ref.get(); ic && ic->hasFocus() &&
              !ic->capabilityFlags().test(CapabilityFlag::Password) &&
              !ic->capabilityFlags().test(CapabilityFlag::Sensitive))
            send(ic, "open");
      }
      return;
    }
    auto id = wire::number(j.get(), "session");
    auto it = contexts_.find(id);
    if (it == contexts_.end())
      return;
    auto *ic = it->second.get();
    if (!ic) {
      contexts_.erase(it);
      return;
    }
    auto *s = state(ic);
    auto seq = uint64_t(wire::number(j.get(), "seq"));
    if (seq <= s->applied)
      return;
    s->applied = seq;
    activity_ = now(CLOCK_MONOTONIC);
    if (uint64_t(wire::number(j.get(), "epoch")) != s->epoch || !ic->hasFocus())
      return;
    auto *commits = wire::get(j.get(), "commits");
    if (commits && json_object_is_type(commits, json_type_array))
      for (size_t i = 0; i < json_object_array_length(commits); i++) {
        auto *v = json_object_array_get_idx(commits, i);
        if (json_object_is_type(v, json_type_string))
          ic->commitString(json_object_get_string(v));
      }
    if (seq != s->seq)
      return;
    s->preedit = wire::str(j.get(), "preedit");
    s->revision = wire::number(j.get(), "revision");
    auto list = std::make_unique<CommonCandidateList>();
    list->setPageSize(pageSize_);
    list->setSelectionKey({Key("1"), Key("2"), Key("3"), Key("4"), Key("5"),
                           Key("6"), Key("7"), Key("8"), Key("9")});
    list->setLayoutHint(vertical_ ? CandidateLayoutHint::Vertical
                                  : CandidateLayoutHint::Horizontal);
    auto *candidates = wire::get(j.get(), "candidates");
    if (candidates && json_object_is_type(candidates, json_type_array))
      for (size_t i = 0;
           i < std::min(size_t(50), json_object_array_length(candidates));
           i++) {
        auto *v = json_object_array_get_idx(candidates, i);
        if (json_object_is_type(v, json_type_string))
          list->append<Word>(this, json_object_get_string(v), i, s->revision);
      }
    if (list->totalSize()) {
      list->setGlobalCursorIndex(0);
      ic->inputPanel().setCandidateList(std::move(list));
    } else
      ic->inputPanel().setCandidateList(nullptr);
    panel(ic, s);
  }

public:
  explicit WeType(Instance *instance)
      : instance_(instance), factory_([this](InputContext &ic) {
          return new State(++nextId_, ic);
        }) {
    reloadSettings();
    instance_->inputContextManager().registerProperty("wetypexState", &factory_);
    settingsAction_.setShortText("WeTypeX 设置");
    settingsAction_.setIcon("fcitx5-wetypex");
    settingsAction_.registerAction("wetypex-settings",
                                   &instance_->userInterfaceManager());
    settingsConnection_ = settingsAction_.connect<SimpleAction::Activated>(
        [](InputContext *) { startProcess({WETYPE_SETTINGS}); });
    timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 100000, 0,
        [this](auto *source, uint64_t time) {
          bool pending = false;
          for (auto &[id, ref] : contexts_)
            if (auto *ic = ref.get()) {
              auto *s = ic->propertyFor(&factory_);
              pending |= s->seq > s->applied;
              if (networkEnabled_ && ready_ && ic->hasFocus() &&
                  !s->preedit.empty() && s->seq == s->applied &&
                  time < s->cloudPollUntil &&
                  time >= s->lastCloudPoll + 100000) {
                s->lastCloudPoll = time;
                send(ic, "poll");
              }
            }
          if (child_ > 0 && (!ready_ || pending) &&
              time >= activity_ + (ready_ ? 30000000 : 60000000))
            fail();
          if (child_ <= 0 && restartAt_ && time >= restartAt_) {
            restartAt_ = 0;
            if (!start())
              scheduleRestart();
          }
          if (time >= syncTick_ + 1000000) {
            syncTick_ = time;
            receiveRemoteClipboard();
            receiveVoice();
          }
          source->setTime(time + 100000);
          source->setEnabled(true);
          return true;
        });
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  const Configuration *getConfig() const override { return &config_; }
  const Configuration *
  getConfigForInputMethod(const InputMethodEntry &) const override {
    return &config_;
  }
  void setConfig(const RawConfig &raw) override {
    config_.load(raw, true);
    saveNativeConfig();
    applyAppearance();
    applyDeviceFunctions();
    stop();
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  void setConfigForInputMethod(const InputMethodEntry &,
                               const RawConfig &raw) override {
    setConfig(raw);
  }
  void reloadConfig() override {
    reloadSettings();
    stop();
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  ~WeType() override {
    timer_.reset();
    stop();
    if (syncChild_ > 0) {
      kill(syncChild_, SIGTERM);
      while (waitpid(syncChild_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
  void activate(const InputMethodEntry &, InputContextEvent &e) override {
    reloadSettings();
    if (networkEnabled_) {
      startSync();
    }
    auto *ic = e.inputContext();
    auto *current = state(ic);
    current->english = *config_.input->defaultLanguage ==
                       wetype_config::DefaultLanguage::English;
    current->traditional = *config_.shortcuts->traditionalSwitch;
    ic->statusArea().addAction(StatusGroup::InputMethod, &settingsAction_);
    if (ic->capabilityFlags().test(CapabilityFlag::Password) ||
        ic->capabilityFlags().test(CapabilityFlag::Sensitive))
      return;
    send(ic, "open");
  }
  void reset(const InputMethodEntry &, InputContextEvent &e) override {
    auto *ic = e.inputContext();
    auto *s = state(ic);
    ++s->epoch;
    s->preedit.clear();
    s->cloudPollUntil = 0;
    ic->inputPanel().reset();
    if (child_ > 0)
      send(ic, "reset");
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  void deactivate(const InputMethodEntry &entry,
                  InputContextEvent &e) override {
    reset(entry, e);
    if (child_ > 0)
      send(e.inputContext(), "close");
  }
  void select(InputContext *ic, unsigned index, int64_t revision = -1) {
    send(ic, "select", "", index, revision);
    auto *s = state(ic);
    s->preedit.clear();
    ic->inputPanel().setCandidateList(nullptr);
    panel(ic, s);
  }
  void keyEvent(const InputMethodEntry &, KeyEvent &e) override {
    auto key = e.key();
    auto *ic = e.inputContext();
    auto *s = state(ic);
    auto sym = key.sym();
    const bool shiftModifier = sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R;
    const bool ctrlModifier = sym == FcitxKey_Control_L || sym == FcitxKey_Control_R;
    if (e.isRelease()) {
      auto releasedConfiguredKey = [&](const KeyList &keys) {
        return std::any_of(keys.begin(), keys.end(),
                           [&](const Key &candidate) {
                             return key.isReleaseOfModifier(candidate);
                           });
      };
      if (voiceRecording_ && voiceHold_ &&
          releasedConfiguredKey(*config_.voice->holdKey)) {
        stopVoice();
        e.filterAndAccept();
        return;
      }
      if (!s->preedit.empty() && s->modifierCandidate == sym &&
          ((*config_.shortcuts->selectCtrl && ctrlModifier) ||
           releasedConfiguredKey(*config_.shortcuts->secondCandidateKeys) ||
           releasedConfiguredKey(*config_.shortcuts->thirdCandidateKeys))) {
        const bool third =
            sym == FcitxKey_Control_R ||
            releasedConfiguredKey(*config_.shortcuts->thirdCandidateKeys);
        select(ic, third ? 2 : 1);
        s->modifierCandidate = FcitxKey_None;
        e.filterAndAccept();
        return;
      }
      if (s->modifierCandidate == sym &&
          ((shiftModifier && *config_.shortcuts->shiftSwitch) ||
           (ctrlModifier && *config_.shortcuts->ctrlSwitch) ||
           releasedConfiguredKey(*config_.shortcuts->languageSwitchKeys))) {
        if (!s->preedit.empty())
          send(ic, "reset");
        s->preedit.clear();
        s->english = !s->english;
        s->modifierCandidate = FcitxKey_None;
        ic->inputPanel().reset();
        panel(ic, s);
        e.filterAndAccept();
      }
      return;
    }
    if (*config_.voice->launchShortcut &&
        key.checkKeyList(*config_.voice->launchKey)) {
      if (!voiceRecording_)
        startVoice(ic, false);
      else
        voiceHold_ = false;
      e.filterAndAccept();
      return;
    }
    if (!voiceRecording_ && *config_.voice->holdShortcut &&
        key.checkKeyList(*config_.voice->holdKey)) {
      startVoice(ic, true);
      e.filterAndAccept();
      return;
    }
    if (voiceRecording_ && !key.isModifier()) {
      stopVoice();
      e.filterAndAccept();
      return;
    }
    if (key.isModifier()) {
      s->modifierCandidate = sym;
      return;
    }
    s->modifierCandidate = FcitxKey_None;
    if (*config_.shortcuts->punctuationSwitch &&
        key.checkKeyList(*config_.shortcuts->punctuationSwitchKeys)) {
      s->englishPunctuation = !s->englishPunctuation;
      e.filterAndAccept();
      return;
    }
    if (*config_.shortcuts->traditionalSwitch &&
        key.checkKeyList(*config_.shortcuts->traditionalSwitchKeys)) {
      s->traditional = !s->traditional;
      if (child_ > 0) {
        send(ic, "close");
        send(ic, "open");
      }
      e.filterAndAccept();
      return;
    }
    if (*config_.shortcuts->halfFull &&
        key.checkKeyList(*config_.shortcuts->halfFullKeys)) {
      s->fullWidth = !s->fullWidth;
      e.filterAndAccept();
      return;
    }
    if (key.states().test(KeyState::Ctrl) || key.states().test(KeyState::Alt) ||
        key.states().test(KeyState::Super))
      return;
    if (ic->capabilityFlags().test(CapabilityFlag::Password) ||
        ic->capabilityFlags().test(CapabilityFlag::Sensitive))
      return;
    if (s->preedit.empty() && *config_.shortcuts->aiAssistant &&
        key.checkKeyList(*config_.shortcuts->aiAssistantKeys)) {
      const auto &surrounding = ic->surroundingText();
      if (surrounding.isValid() && surrounding.cursor()) {
        const auto &all = surrounding.text();
        auto byteCursor = utf8::ncharByteLength(all.begin(), surrounding.cursor());
        if (byteCursor > 0 && size_t(byteCursor) <= all.size()) {
          std::string question = all.substr(0, size_t(byteCursor));
          constexpr size_t MaxContextBytes = 4096;
          if (question.size() > MaxContextBytes) {
            size_t begin = question.size() - MaxContextBytes;
            while (begin < question.size() &&
                   (static_cast<unsigned char>(question[begin]) & 0xc0) == 0x80)
              ++begin;
            question.erase(0, begin);
          }
          while (!question.empty() &&
                 std::isspace(static_cast<unsigned char>(question.back())))
            question.pop_back();
          if (!question.empty()) {
            startProcess({WETYPE_AI, question});
            e.filterAndAccept();
            return;
          }
        }
      }
    }
    if (s->english) {
      if (s->fullWidth && sym >= 0x20 && sym <= 0x7e) {
        uint32_t full = sym == FcitxKey_space ? 0x3000 : sym + 0xfee0;
        ic->commitString(utf8::UCS4ToUTF8(full));
        e.filterAndAccept();
      }
      return;
    }
    bool composing = !s->preedit.empty();
    if (!composing)
      recordClipboard(ic);
    if (failed_) {
      if (composing && sym == FcitxKey_Return) {
        ic->commitString(s->preedit);
        s->preedit.clear();
        ic->inputPanel().reset();
        panel(ic, s);
        e.filterAndAccept();
      }
      return;
    }
    auto *list = dynamic_cast<CommonCandidateList *>(
        ic->inputPanel().candidateList().get());
    bool pageDown = sym == FcitxKey_Page_Down ||
                    key.checkKeyList(*config_.shortcuts->nextPageKeys) ||
                    (*config_.shortcuts->pageMinusEqual &&
                     sym == FcitxKey_equal) ||
                    (*config_.shortcuts->pageBrackets &&
                     sym == FcitxKey_bracketright) ||
                    (*config_.shortcuts->pageCommaPeriod &&
                     sym == FcitxKey_period) ||
                    (*config_.shortcuts->pageShiftTab && sym == FcitxKey_Tab);
    bool pageUp = sym == FcitxKey_Page_Up ||
                  key.checkKeyList(*config_.shortcuts->previousPageKeys) ||
                  (*config_.shortcuts->pageMinusEqual &&
                   sym == FcitxKey_minus) ||
                  (*config_.shortcuts->pageBrackets &&
                   sym == FcitxKey_bracketleft) ||
                  (*config_.shortcuts->pageCommaPeriod &&
                   sym == FcitxKey_comma) ||
                  (*config_.shortcuts->pageShiftTab &&
                   key.states().test(KeyState::Shift) && sym == FcitxKey_Tab);
    if (composing && (pageDown || pageUp)) {
      if (list) {
        if (pageDown) {
          if (list->hasNext())
            list->next();
        } else if (list->hasPrev())
          list->prev();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
      }
      e.filterAndAccept();
      return;
    }
    const bool configuredSecond =
        key.checkKeyList(*config_.shortcuts->secondCandidateKeys);
    const bool configuredThird =
        key.checkKeyList(*config_.shortcuts->thirdCandidateKeys);
    if (composing &&
        (configuredSecond || configuredThird ||
         (*config_.shortcuts->selectSemicolonQuote &&
          (sym == FcitxKey_semicolon || sym == FcitxKey_apostrophe)))) {
      select(ic, configuredSecond || sym == FcitxKey_semicolon ? 1 : 2);
      e.filterAndAccept();
      return;
    }
    if (composing && (sym == FcitxKey_space ||
                      (sym >= FcitxKey_1 && sym < FcitxKey_1 + pageSize_))) {
      unsigned index = (list ? list->currentPage() * pageSize_ : 0) +
                       (sym == FcitxKey_space ? 0 : sym - FcitxKey_1);
      select(ic, index);
      e.filterAndAccept();
      return;
    }
    if (composing && sym == FcitxKey_BackSpace) {
      if (!s->preedit.empty()) {
        size_t pos = s->preedit.size() - 1;
        while (pos &&
               (static_cast<unsigned char>(s->preedit[pos]) & 0xc0) == 0x80)
          --pos;
        s->preedit.erase(pos);
      }
      send(ic, "backspace");
      panel(ic, s);
      e.filterAndAccept();
      return;
    }
    if (composing && (sym == FcitxKey_Escape || sym == FcitxKey_Return ||
                      sym == FcitxKey_KP_Enter)) {
      send(ic, sym == FcitxKey_Escape ? "reset" : "raw");
      s->preedit.clear();
      ic->inputPanel().setCandidateList(nullptr);
      panel(ic, s);
      e.filterAndAccept();
      return;
    }
    bool letter = (sym >= FcitxKey_a && sym <= FcitxKey_z) ||
                  (sym >= FcitxKey_A && sym <= FcitxKey_Z);
    bool punctuation = sym == FcitxKey_comma || sym == FcitxKey_period ||
                       sym == FcitxKey_semicolon || sym == FcitxKey_colon ||
                       sym == FcitxKey_question || sym == FcitxKey_exclam ||
                       sym == FcitxKey_apostrophe ||
                       (sym == FcitxKey_slash && slashPunctuation_);
    if (!composing && *config_.input->symbolAutoPair) {
      const char *pair = nullptr;
      if (sym == FcitxKey_parenleft)
        pair = chinesePunctuation_ ? "（）" : "()";
      else if (sym == FcitxKey_braceleft)
        pair = chinesePunctuation_ ? "｛｝" : "{}";
      else if (sym == FcitxKey_bracketleft)
        pair = chinesePunctuation_ ? "【】" : "[]";
      if (pair) {
        ic->commitStringWithCursor(pair, 1);
        e.filterAndAccept();
        return;
      }
    }
    if (!composing && symbolAutoChange_ && sym == FcitxKey_colon) {
      const auto &surrounding = ic->surroundingText();
      if (surrounding.isValid() && surrounding.cursor()) {
        const auto &text = surrounding.text();
        auto bytes = utf8::ncharByteLength(text.begin(), surrounding.cursor());
        if (bytes > 0 && size_t(bytes) <= text.size() &&
            text[size_t(bytes) - 1] >= '0' && text[size_t(bytes) - 1] <= '9') {
          ic->commitString(":");
          e.filterAndAccept();
          return;
        }
      }
    }
    if (letter || punctuation) {
      std::string text(1, char(sym));
      s->preedit += text;
      if (letter && networkEnabled_)
        s->cloudPollUntil = now(CLOCK_MONOTONIC) + 1500000;
      bool oldPunctuation = chinesePunctuation_;
      if (s->englishPunctuation)
        chinesePunctuation_ = false;
      send(ic, punctuation ? "punctuation" : "key", text);
      chinesePunctuation_ = oldPunctuation;
      panel(ic, s);
      e.filterAndAccept();
    }
  }
};
void Word::select(InputContext *ic) const {
  engine_->select(ic, index_, revision_);
}
class Factory : public AddonFactory {
  AddonInstance *create(AddonManager *m) override {
    return new WeType(m->instance());
  }
};
} // namespace
FCITX_ADDON_FACTORY(Factory)
