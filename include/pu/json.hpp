// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Thin convenience layer over Boost.JSON used throughout pu-cli.  It keeps the
// handful of nlohmann-style lookups (value-with-default, has-key, shallow
// merge, pretty printing) readable while all JSON storage/parsing is handled
// by boost::json.

#include <boost/json.hpp>

#include <cstddef>
#include <string>

namespace pu {
namespace json {

using value = boost::json::value;
using object = boost::json::object;
using array = boost::json::array;
using string = boost::json::string;

// Parsing/serialization entry points (boost::json::parse throws
// boost::system::system_error on malformed input).
using boost::json::parse;
using boost::json::serialize;

// Return the member `key` of `j` converted to `T`, or `def` when `j` is not an
// object, the member is absent, or the member is `null`.
template <class T>
T ValueOrDefault(const value& j, boost::json::string_view key, const T& def) {
  const object* obj = j.if_object();
  if (!obj) return def;
  auto it = obj->find(key);
  if (it == obj->end()) return def;
  return boost::json::value_to<T>(it->value());
}

// nlohmann::json::value("key", "literal") returns a std::string.
inline std::string ValueOrDefault(const value& j, boost::json::string_view key,
                                  const char* def) {
  return ValueOrDefault<std::string>(j, key, std::string(def));
}

// True when `j` is an object containing `key`.
inline bool HasKey(const value& j, boost::json::string_view key) {
  const object* obj = j.if_object();
  return obj != nullptr && obj->contains(key);
}

// Shallow merge of `src`'s members into `dst` (mirrors nlohmann::json::update).
inline void Merge(value& dst, const value& src) {
  if (!dst.is_object() || !src.is_object()) return;
  for (const auto& kv : src.as_object()) {
    dst.as_object()[kv.key()] = kv.value();
  }
}

namespace detail {

inline void AppendPretty(const value& jv, std::string& out, int depth,
                         int indent) {
  std::string pad(static_cast<std::size_t>(depth) * static_cast<std::size_t>(indent), ' ');
  std::string member_pad(static_cast<std::size_t>(depth + 1) *
                             static_cast<std::size_t>(indent),
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

}  // namespace detail

// Serialize `jv` with pretty printing using `indent` spaces per level
// (comparable to nlohmann::json::dump(indent)).
inline std::string PrettyPrint(const value& jv, int indent = 2) {
  std::string out;
  detail::AppendPretty(jv, out, 0, indent);
  return out;
}

}  // namespace json
}  // namespace pu
