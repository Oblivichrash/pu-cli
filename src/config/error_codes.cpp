// SPDX-License-Identifier: GPL-3.0-only
#include "pu/error_codes.hpp"

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

const ConfigCategory kConfigCategory{};
const HttpCategory   kHttpCategory{};
const StoreCategory  kStoreCategory{};
}  // namespace

std::error_code make_error_code(ConfigErrc e) { return {static_cast<int>(e), kConfigCategory}; }
std::error_code make_error_code(HttpErrc e)   { return {static_cast<int>(e), kHttpCategory}; }
std::error_code make_error_code(StoreErrc e)  { return {static_cast<int>(e), kStoreCategory}; }

}  // namespace pu
