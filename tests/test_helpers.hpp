// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace test_helpers {

inline void set_env(const char* name, const char* value, int overwrite) {
#ifdef _WIN32
  (void)overwrite;
  _putenv_s(name, value ? value : "");
#else
  setenv(name, value, overwrite);
#endif
}

inline void unset_env(const char* name) {
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

}  // namespace test_helpers