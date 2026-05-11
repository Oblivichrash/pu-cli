// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <system_error>

namespace pu {

enum class ConfigErrc {
  file_not_found = 1,
  parse_error,
  missing_field,
  backend_unknown
};

enum class HttpErrc {
  connection_failed = 1,
  http_error,
  interrupted
};

enum class StoreErrc {
  not_found = 1,
  invalid_data,
  write_failed
};

std::error_code make_error_code(ConfigErrc e);
std::error_code make_error_code(HttpErrc e);
std::error_code make_error_code(StoreErrc e);

}  // namespace pu

namespace std {
template <> struct is_error_code_enum<pu::ConfigErrc> : true_type {};
template <> struct is_error_code_enum<pu::HttpErrc> : true_type {};
template <> struct is_error_code_enum<pu::StoreErrc> : true_type {};
}  // namespace std
