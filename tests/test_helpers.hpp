// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdlib>
#include <string>

namespace test_helpers {

inline void set_env(const char* name, const char* value) {
#ifdef _WIN32
  _putenv((std::string(name) + "=" + value).c_str());
#else
  setenv(name, value, 1);
#endif
}

inline void unset_env(const char* name) {
#ifdef _WIN32
  _putenv((std::string(name) + "=").c_str());
#else
  unsetenv(name);
#endif
}

}  // namespace test_helpers