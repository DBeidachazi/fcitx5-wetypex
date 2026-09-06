// Native engine IPC. All engine operations are serialized on the service
// thread.
#include "control.inc"
// Verified against +[WXIMEUtil addHotWord:value:] and removeHotWord:.
// The 80-byte aggregate is passed BY VALUE, including in the enumerator
// callback.
struct HotWord {
  const char *id;
  uint32_t idlen, pad0;
  const char *key;
  uint32_t keylen, pad1;
  const char *words;
  uint32_t wordslen, flag;
  const char *file;
  uint32_t filelen, pad2;
  const char *thumb;
  uint32_t thumblen, kind;
};
static_assert(sizeof(HotWord) == 80);
static bool hotword_collect(void *opaque, HotWord item) {
  auto *array = static_cast<json_object *>(opaque);
  if (json_object_array_length(array) >= 10000)
    return false;
  auto row = wire::object();
  for (auto field : {std::tuple{"id", item.id, item.idlen},
                     std::tuple{"key", item.key, item.keylen},
                     std::tuple{"words", item.words, item.wordslen}}) {
    auto [name, data, size] = field;
    if (size > 65536 || (!data && size))
      _exit(89);
    wire::put(row.get(), name, std::string(data ? data : "", size));
  }
  wire::put(row.get(), "flag", int64_t(item.flag));
  wire::put(row.get(), "kind", int64_t(item.kind));
  json_object_array_add(array, row.release());
  return true;
}
struct ServiceCandidate {
  std::string text;
  std::vector<unsigned char> id;
  uint32_t cover = 0;
};
struct ServiceSession {
  uint64_t engine = 0, revision = 0;
  uint32_t keyboard = 0;
  std::string preedit, selected;
  unsigned pending = 0;
  std::mutex mutex;
  std::vector<ServiceCandidate> candidates;
  std::vector<std::string> commits;
};
static void configure_cloud_session(unsigned char *config) {
  // Windows 2.1.3.18 desktop defaults recovered from wetype_server.exe's
  // SessionConfigRW fallback object and its field serializer.  In particular,
  // the desktop enables cloud candidates while typing, but keeps the mobile-
  // style post-commit "most likely" list disabled.
  config[0x41] = 1; // cloud.enable_cloud_search_by_input
  *(uint32_t *)(config + 0x50) = 3; // cloud.process_input.return_cnt
  *(uint32_t *)(config + 0x54) = 1; // cloud.process_input.predict_next_n
}
static void read_service_candidates(void *iterator,
                                    std::vector<ServiceCandidate> &output) {
  output.clear();
  for (unsigned rank = 0; iterator && rank < 50; rank++) {
    alignas(16) unsigned char c[512]{};
    if (!((bool (*)(void *, void *))syms.at("_wxime_candidate_next"))(
            iterator, c))
      break;
    const char *text;
    const unsigned char *id;
    uint32_t len, n;
    memcpy(&text, c, 8);
    memcpy(&len, c + 8, 4);
    memcpy(&id, c + 0x20, 8);
    memcpy(&n, c + 0x28, 4);
    if (!text || len > 65536 || n > 65536)
      _exit(89);
    ServiceCandidate candidate;
    candidate.text.assign(text, len);
    memcpy(&candidate.cover, c + 0x64, 4);
    if (id && n)
      candidate.id.assign(id, id + n);
    output.push_back(std::move(candidate));
  }
}
static void service_listener(uint64_t, int32_t *types, void **payloads,
                             uint32_t count, void *opaque) {
  auto &s = *(ServiceSession *)opaque;
  if (count > 64 || (!types && count) || (!payloads && count))
    _exit(89);
  std::lock_guard<std::mutex> guard(s.mutex);
  for (unsigned i = 0; i < count; i++) {
    if (!payloads[i])
      continue;
    if (types[i] == 0) {
      // CCallbackHolder::ParseEvent: iterator +0, kind +8,
      // cloud iterator +16, version +24 (macOS 2.2.3.657).
      auto it = *(void **)payloads[i];
      auto cloud = *((void **)payloads[i] + 2);
      read_service_candidates(it, s.candidates);
      if (it)
        ((void (*)(void *))syms.at("_wxime_delete_candidate_iterator"))(it);
      if (cloud && cloud != it)
        ((void (*)(void *))syms.at("_wxime_delete_candidate_iterator"))(cloud);
    } else if (types[i] == 6) {
      // _wxime_cloud_result_event: iterator +0, kind +8, version +24.
      // The iterator is already the original engine's fully merged list.
      auto it = *(void **)payloads[i];
      read_service_candidates(it, s.candidates);
      if (it)
        ((void (*)(void *))syms.at("_wxime_delete_candidate_iterator"))(it);
    } else if (types[i] == 2) {
      const char *text = *(const char **)payloads[i];
      uint32_t n = *(uint32_t *)((char *)payloads[i] + 8);
      if (text && n < 65536) {
        s.commits.emplace_back(text, n);
        s.selected.clear();
      }
    } else if (types[i] == 4) {
      s.pending = *(uint32_t *)((char *)payloads[i] + 8);
      if (s.pending > 4096)
        _exit(89);
    }
  }
}
static void service_select(ServiceSession &s, unsigned rank) {
  ServiceCandidate c;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (rank >= s.candidates.size())
      return;
    c = s.candidates[rank];
  }
  using Fn = void (*)(uint64_t, const char *, uint32_t, const void *, uint32_t,
                      const char *, uint32_t, const void *, uint32_t,
                      const void *, uint32_t, const void *);
  ((Fn)syms.at("_wxime_select_candidate"))(
      s.engine, c.text.data(), c.text.size(), c.id.data(), c.id.size(), nullptr,
      0, nullptr, 0, nullptr, 0, nullptr);
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (c.cover <= s.preedit.size())
      s.preedit.erase(0, c.cover);
    if (s.pending && s.commits.empty())
      s.selected += c.text;
    if (!s.pending) {
      s.preedit.clear();
      s.selected.clear();
    }
  }
}
static void service_loop() {
  std::map<uint64_t, std::unique_ptr<ServiceSession>> sessions;
  ServiceTransport transport;
  puts("{\"event\":\"ready\",\"protocol\":1,\"backend\":\"original-wetype-"
       "native\"}");
  fflush(stdout);
  std::string line;
  int source = 0;
  while (transport.next(line, source)) {
    if (line.size() > 65536)
      _exit(90);
    auto request = wire::parse(line);
    if (!request || !json_object_is_type(request.get(), json_type_object)) {
      if (source == 0)
        _exit(90);
      transport.reply(source, "{\"error\":\"invalid JSON request\"}");
      continue;
    }
    auto op = wire::str(request.get(), "op");
    auto client = wire::number(request.get(), "session");
    auto seq = wire::number(request.get(), "seq");
    auto epoch = wire::number(request.get(), "epoch");
    if (source != 0 && op != "hotword_list" && op != "hotword_set") {
      transport.reply(source,
                      "{\"error\":\"management operation not allowed\"}");
      continue;
    }
    auto response = wire::object();
    wire::put(response.get(), "session", client);
    wire::put(response.get(), "seq", seq);
    wire::put(response.get(), "epoch", epoch);
    if (op == "quit")
      break;
    if (client <= 0 || seq < 0 || sessions.size() > 64) {
      wire::put(response.get(), "error", std::string("invalid session"));
    } else if (op == "hotword_list" || op == "hotword_set") {
      if (op == "hotword_set") {
        auto id = wire::str(request.get(), "id"),
             key = wire::str(request.get(), "key"),
             words = wire::str(request.get(), "words");
        if (id.size() > 256 || key.size() > 256 || words.size() > 16384 ||
            (id.empty() && words.empty()))
          wire::put(response.get(), "error", std::string("invalid hotword"));
        else {
          HotWord item{};
          item.id = id.empty() ? nullptr : id.data();
          item.idlen = id.size();
          item.key = key.empty() ? nullptr : key.data();
          item.keylen = key.size();
          item.words = words.empty() ? nullptr : words.data();
          item.wordslen = words.size();
          item.flag = 0; // Do not transplant Android enum values into this ABI.
          bool ok =
              ((bool (*)(HotWord))syms.at("_wxime_set_user_hot_word"))(item);
          if (!ok)
            wire::put(response.get(), "error",
                      std::string("original engine rejected hotword"));
        }
      }
      auto *array = json_object_new_array();
      ((void (*)(decltype(&hotword_collect), void *))syms.at(
          "_wxime_get_user_hot_word"))(hotword_collect, array);
      json_object_object_add(response.get(), "hotwords", array);
    } else if (op == "close") {
      auto it = sessions.find(client);
      if (it != sessions.end()) {
        ((void (*)(uint64_t))syms.at("_wxime_destroy_session"))(
            it->second->engine);
        sessions.erase(it);
      }
    } else {
      if (!sessions.count(client)) {
        auto state = std::make_unique<ServiceSession>();
        alignas(16) unsigned char config[2048]{};
        config[1] = 1;
        if (getenv("WETYPE_NETWORK_LIVE"))
          configure_cloud_session(config);
        uint32_t keyboard =
            wire::number(request.get(), "keyboard", 0) == 5 ? 5 : 0;
        memcpy(config + 4, &keyboard, sizeof(keyboard));
        uint32_t doublePin = std::clamp<int>(
            wire::number(request.get(), "double_scheme", 0), 0, 7);
        memcpy(config + 0x20, &doublePin, sizeof(doublePin));
        if (wire::number(request.get(), "smart_input", 1)) {
          // InputAugmenter: transpose, neighbouring-key, insertion and skip
          // correction. Offsets are recovered from the original JNI bridge.
          for (unsigned offset : {0x8u, 0x9u, 0xau, 0xbu})
            config[offset] = 1;
        }
        config[0x0d] = wire::number(request.get(), "fuzzy_nl", 0);
        config[0x0e] = wire::number(request.get(), "fuzzy_rl", 0);
        config[0x0f] = wire::number(request.get(), "fuzzy_hf", 0);
        config[0x10] = wire::number(request.get(), "fuzzy_gk", 0);
        config[0x14] = wire::number(request.get(), "fuzzy_c_ch", 0);
        config[0x15] = wire::number(request.get(), "fuzzy_s_sh", 0);
        config[0x16] = wire::number(request.get(), "fuzzy_z_zh", 0);
        if (wire::number(request.get(), "emoji_recommend", 1)) {
          config[0x33] = 1; // WeChat emoji
          config[0x34] = 1; // normal emoji
          config[0x35] = 1; // kaomoji
          config[0x36] = 1; // emoji plus
          config[0x39] = 1; // symbol emoji
        }
        *(uint32_t *)(config + 0xb8) =
            wire::number(request.get(), "traditional", 0) ? 1 : 0;
        state->keyboard = keyboard;
        state->engine =
            ((uint64_t (*)(void *))syms.at("_wxime_create_session"))(config);
        if (!state->engine)
          _exit(91);
        ((void (*)(uint64_t, decltype(&service_listener), void *))syms.at(
            "_wxime_add_session_listener"))(state->engine, service_listener,
                                            state.get());
        sessions[client] = std::move(state);
      }
      auto &s = *sessions.at(client);
      if (op == "predict") {
        auto before = wire::str(request.get(), "before");
        if (before.size() > 4096 || s.pending)
          wire::put(response.get(), "error",
                    std::string("invalid prediction context"));
        else {
          using Surround =
              void (*)(uint64_t, const char *, uint32_t, const char *, uint32_t,
                       const char *, uint32_t, int);
          ((Surround)syms.at("_wxime_set_text_around_cursor"))(
              s.engine, before.data(), before.size(), "", 0, "", 0, 0);
          ((void (*)(uint64_t))syms.at("_wxime_get_most_likely_sequel"))(
              s.engine);
        }
      } else if (op == "key") {
        auto key = wire::str(request.get(), "key");
        if (key.size() != 1 || (unsigned char)key[0] < 32 ||
            (unsigned char)key[0] > 126)
          _exit(90);
        if (s.preedit.size() >= 128 || s.selected.size() >= 384)
          wire::put(response.get(), "error", std::string("composition limit"));
        else {
          s.preedit += key;
          ((void (*)(uint64_t, const char *, uint32_t, const void *,
                     uint32_t))syms.at("_wxime_process_input"))(
              s.engine, key.data(), 1, nullptr, 0);
        }
      } else if (op == "punctuation") {
        static const std::map<std::string, std::string> map = {
            {",", "，"}, {".", "。"}, {";", "；"}, {":", "："},
            {"?", "？"}, {"!", "！"}, {"'", "'"}, {"/", "、"}};
        auto key = wire::str(request.get(), "key");
        auto it = map.find(key);
        if (it == map.end())
          _exit(90);
        if (!s.preedit.empty() || !s.selected.empty())
          service_select(s, 0);
        if (!s.preedit.empty() || !s.selected.empty()) {
          auto raw = s.selected + s.preedit;
          ((void (*)(uint64_t))syms.at("_wxime_reset_session"))(s.engine);
          std::lock_guard<std::mutex> lock(s.mutex);
          s.commits.push_back(raw);
          s.preedit.clear();
          s.selected.clear();
          s.pending = 0;
          s.candidates.clear();
        }
        {
          std::lock_guard<std::mutex> lock(s.mutex);
          s.commits.push_back(
              wire::number(request.get(), "chinese_punctuation", 1) ? it->second
                                                                    : key);
        }
      } else if (op == "backspace") {
        if (!s.preedit.empty())
          s.preedit.pop_back();
        ((void (*)(uint64_t, uint8_t, const void *, uint32_t))syms.at(
            "_wxime_drop_last"))(s.engine, 1, nullptr, 0);
      } else if (op == "select") {
        auto expected = wire::number(request.get(), "revision", -1);
        if (expected >= 0 && (uint64_t)expected != s.revision)
          wire::put(response.get(), "error", std::string("stale candidate"));
        else
          service_select(s, wire::number(request.get(), "index"));
      } else if (op == "reset" || op == "raw") {
        auto raw = s.selected + s.preedit;
        ((void (*)(uint64_t))syms.at("_wxime_reset_session"))(s.engine);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending = 0;
        s.preedit.clear();
        s.selected.clear();
        s.candidates.clear();
        if (op == "raw" && !raw.empty())
          s.commits.push_back(raw);
      } else if (op != "open" && op != "poll")
        wire::put(response.get(), "error", std::string("unknown operation"));
      {
        std::lock_guard<std::mutex> lock(s.mutex);
        // PendingInput entries can combine letters (e.g. "zh"). Their
        // count is not the number of typed ASCII bytes.
        if (!s.pending && (!s.keyboard || !s.commits.empty())) {
          s.preedit.clear();
          s.selected.clear();
        }
        s.revision = seq;
        wire::put(response.get(), "revision", seq);
        wire::put(response.get(), "preedit", s.selected + s.preedit);
        auto *array = json_object_new_array();
        for (auto &c : s.candidates)
          json_object_array_add(
              array, json_object_new_string_len(c.text.data(), c.text.size()));
        json_object_object_add(response.get(), "candidates", array);
        array = json_object_new_array();
        for (auto &c : s.commits)
          json_object_array_add(array,
                                json_object_new_string_len(c.data(), c.size()));
        json_object_object_add(response.get(), "commits", array);
        s.commits.clear();
      }
    }
    transport.reply(source, wire::dump(response.get()));
  }
  for (auto &[client, s] : sessions)
    ((void (*)(uint64_t))syms.at("_wxime_destroy_session"))(s->engine);
  ((void (*)())syms.at("_wxime_finalize"))();
}
