// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/json.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <string>

namespace pu {
namespace json {

using value = boost::json::value;
using object = boost::json::object;
using array = boost::json::array;
using string = boost::json::string;

// boost::json::parse throws boost::system::system_error on malformed input.
using boost::json::parse;
using boost::json::serialize;

// `j`'s member `key` as `T`, or `def` when `j` is not an object, the member is absent or
// null, or it holds another type — a file written elsewhere can hold a differing type.
template <class T>
T ValueOrDefault(const value& j, boost::json::string_view key, const T& def) {
  const object* obj = j.if_object();
  if (!obj) return def;
  auto it = obj->find(key);
  if (it == obj->end()) return def;
  try {
    return boost::json::value_to<T>(it->value());
  } catch (const std::exception&) {
    return def;
  }
}

// So ValueOrDefault(j, "key", "literal") yields a std::string.
inline std::string ValueOrDefault(const value& j, boost::json::string_view key, const char* def) {
  return ValueOrDefault<std::string>(j, key, std::string(def));
}

inline bool HasKey(const value& j, boost::json::string_view key) {
  const object* obj = j.if_object();
  return obj != nullptr && obj->contains(key);
}

// `j`'s member `key` as `T`, or nullopt when it is absent, null, or holds another type.
// For a setting where "absent" must stay distinguishable from any value of `T` — an
// absent token cap is not the same as a cap of zero.
template <class T>
std::optional<T> OptionalOrDefault(const value& j, boost::json::string_view key) {
  if (!HasKey(j, key)) return std::nullopt;
  try {
    return boost::json::value_to<T>(j.at(key));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// `error` as text or object, else a top-level `message` or `msg` (CodeBuddy's name).
inline std::string ErrorMessage(const value& j) {
  const object* obj = j.if_object();
  if (obj == nullptr) return {};

  if (auto it = obj->find("error"); it != obj->end()) {
    if (it->value().is_string()) return boost::json::value_to<std::string>(it->value());
    const std::string inner = ValueOrDefault<std::string>(it->value(), "message", "");
    if (!inner.empty()) return inner;
  }
  const std::string message = ValueOrDefault<std::string>(j, "message", "");
  if (!message.empty()) return message;
  return ValueOrDefault<std::string>(j, "msg", "");
}

// The user-facing wording a gateway supplies in place of its own log line. A
// gateway that writes `displayMsg` as an object gets its `en` member; the caller
// falls back to `ErrorMessage` when this is empty.
inline std::string DisplayMessage(const value& j) {
  const object* obj = j.if_object();
  if (obj == nullptr) return {};

  const auto it = obj->find("displayMsg");
  if (it == obj->end()) return {};
  if (it->value().is_string()) return boost::json::value_to<std::string>(it->value());
  return ValueOrDefault<std::string>(it->value(), "en", "");
}

// What to show a user for a rejected request: the gateway's `displayMsg` when it
// supplies one, else whatever names the cause.
inline std::string UserFacingError(const value& j) {
  const std::string display = DisplayMessage(j);
  if (!display.empty()) return display;
  return ErrorMessage(j);
}

// Pretty-prints `jv` into `out`, indenting each level by `indent` spaces.
inline void AppendPretty(const value& jv, std::string& out, int depth, int indent) {
  std::string pad(static_cast<std::size_t>(depth) * static_cast<std::size_t>(indent), ' ');
  std::string member_pad(static_cast<std::size_t>(depth + 1) * static_cast<std::size_t>(indent),
                         ' ');
  if (jv.is_object()) {
    const object& o = jv.as_object();
    if (o.empty()) {
      out += "{}";
      return;
    }
    out += "{\n";
    bool first = true;
    for (const auto& kv : o) {
      if (!first) out += ",\n";
      first = false;
      out += member_pad;
      out += boost::json::serialize(boost::json::string(kv.key()));
      out += ": ";
      AppendPretty(kv.value(), out, depth + 1, indent);
    }
    out += "\n" + pad + "}";
  } else if (jv.is_array()) {
    const array& a = jv.as_array();
    if (a.empty()) {
      out += "[]";
      return;
    }
    out += "[\n";
    bool first = true;
    for (const value& item : a) {
      if (!first) out += ",\n";
      first = false;
      out += member_pad;
      AppendPretty(item, out, depth + 1, indent);
    }
    out += "\n" + pad + "]";
  } else {
    out += boost::json::serialize(jv);
  }
}

// Serialize `jv` with pretty printing using `indent` spaces per level.
inline std::string PrettyPrint(const value& jv, int indent = 2) {
  std::string out;
  AppendPretty(jv, out, 0, indent);
  return out;
}

}  // namespace json
}  // namespace pu
