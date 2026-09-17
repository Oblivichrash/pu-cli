// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdlib>
#include <string>

namespace pu::tests {

// RAII helper to set/unset environment variables for testing.
class ScopedEnvVar {
public:
  ScopedEnvVar(const std::string& name, const std::string& value) : name_(name) {
    const char* prev = std::getenv(name.c_str());
    had_prev_ = (prev != nullptr);
    if (had_prev_) prev_ = prev;
    Set(value);
  }

  ~ScopedEnvVar() {
    if (had_prev_) Set(prev_);
    else Unset();
  }

private:
  void Set(const std::string& value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value.c_str());
#else
    setenv(name_.c_str(), value.c_str(), 1);
#endif
  }

  void Unset() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), "");
#else
    unsetenv(name_.c_str());
#endif
  }

  std::string name_;
  std::string prev_;
  bool had_prev_ = false;
};

}  // namespace pu::tests
