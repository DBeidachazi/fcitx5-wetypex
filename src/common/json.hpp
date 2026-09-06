#pragma once
#include <cstdint>
#include <json-c/json.h>
#include <memory>
#include <string>
namespace wire {
struct Deleter {
  void operator()(json_object *p) const {
    if (p)
      json_object_put(p);
  }
};
using Json = std::unique_ptr<json_object, Deleter>;
inline Json object() { return Json(json_object_new_object()); }
inline Json parse(const std::string &s) {
  json_tokener *t = json_tokener_new();
  auto *p = json_tokener_parse_ex(t, s.data(), s.size());
  bool ok = json_tokener_get_error(t) == json_tokener_success;
  json_tokener_free(t);
  if (!ok && p)
    json_object_put(p);
  return Json(ok ? p : nullptr);
}
inline json_object *get(json_object *p, const char *k) {
  json_object *v = nullptr;
  if (p)
    json_object_object_get_ex(p, k, &v);
  return v;
}
inline std::string str(json_object *p, const char *k,
                       const char *fallback = "") {
  auto *v = get(p, k);
  return v && json_object_is_type(v, json_type_string)
             ? json_object_get_string(v)
             : fallback;
}
inline int64_t number(json_object *p, const char *k, int64_t fallback = 0) {
  auto *v = get(p, k);
  return v && json_object_is_type(v, json_type_int) ? json_object_get_int64(v)
                                                    : fallback;
}
inline void put(json_object *p, const char *k, const std::string &v) {
  json_object_object_add(p, k, json_object_new_string_len(v.data(), v.size()));
}
inline void put(json_object *p, const char *k, int64_t v) {
  json_object_object_add(p, k, json_object_new_int64(v));
}
inline void boolean(json_object *p, const char *k, bool v) {
  json_object_object_add(p, k, json_object_new_boolean(v));
}
inline std::string dump(json_object *p) {
  return json_object_to_json_string_ext(p, JSON_C_TO_STRING_PLAIN);
}
} // namespace wire
