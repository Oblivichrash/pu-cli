// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <system_error>

namespace pu {

enum class ConfigErrc { file_not_found = 1, parse_error, missing_field, backend_unknown };
enum class HttpErrc { connection_failed = 1, http_error, interrupted };
enum class StoreErrc { not_found = 1, invalid_data, write_failed };

}  // namespace pu

namespace std {
template <> struct is_error_code_enum<pu::ConfigErrc> : true_type {};
template <> struct is_error_code_enum<pu::HttpErrc> : true_type {};
template <> struct is_error_code_enum<pu::StoreErrc> : true_type {};
}  // namespace std

namespace pu {

namespace {
struct ConfigCategory : std::error_category {
  const char* name() const noexcept override { return "pu.config"; }
  std::string message(int ev) const override {
    switch (static_cast<ConfigErrc>(ev)) {
      case ConfigErrc::file_not_found: return "Configuration file not found";
      case ConfigErrc::parse_error:    return "Configuration JSON parse error";
      case ConfigErrc::missing_field:  return "Required field missing in configuration";
      case ConfigErrc::backend_unknown: return "Unknown backend type";
      default: return "unknown config error";
    }
  }
};
struct HttpCategory : std::error_category {
  const char* name() const noexcept override { return "pu.http"; }
  std::string message(int ev) const override {
    switch (static_cast<HttpErrc>(ev)) {
      case HttpErrc::connection_failed: return "HTTP connection failed";
      case HttpErrc::http_error:        return "HTTP error response";
      case HttpErrc::interrupted:       return "Request interrupted";
      default: return "unknown HTTP error";
    }
  }
};
struct StoreCategory : std::error_category {
  const char* name() const noexcept override { return "pu.store"; }
  std::string message(int ev) const override {
    switch (static_cast<StoreErrc>(ev)) {
      case StoreErrc::not_found:    return "Conversation not found";
      case StoreErrc::invalid_data: return "Invalid conversation data";
      case StoreErrc::write_failed: return "Failed to write conversation";
      default: return "unknown store error";
    }
  }
};
}  // namespace

inline std::error_code make_error_code(ConfigErrc e) {
  static const ConfigCategory cat{};
  return {static_cast<int>(e), cat};
}
inline std::error_code make_error_code(HttpErrc e) {
  static const HttpCategory cat{};
  return {static_cast<int>(e), cat};
}
inline std::error_code make_error_code(StoreErrc e) {
  static const StoreCategory cat{};
  return {static_cast<int>(e), cat};
}

}  // namespace pu
