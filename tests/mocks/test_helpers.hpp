// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

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
    if (had_prev_)
      Set(prev_);
    else
      Unset();
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

// Runs a block inside `dir`. A Runtime takes its workspace — session file, agents, tools —
// from the working directory, so a test that never enters one shares the repository's.
class ScopedWorkingDir {
 public:
  explicit ScopedWorkingDir(const std::filesystem::path& dir)
      : original_(std::filesystem::current_path()) {
    std::filesystem::current_path(dir);
  }

  ~ScopedWorkingDir() {
    std::error_code ec;
    std::filesystem::current_path(original_, ec);
  }

  ScopedWorkingDir(const ScopedWorkingDir&) = delete;
  ScopedWorkingDir& operator=(const ScopedWorkingDir&) = delete;

 private:
  std::filesystem::path original_;
};

// A directory under the system temp, removed with the object: a test's data directory is not
// the tree it is run from, so it is not one a run may leave behind.
class ScopedTempDir {
 public:
  explicit ScopedTempDir(const std::string& prefix) {
    static std::atomic<int> seq{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (prefix + std::to_string(stamp) + "_" + std::to_string(seq.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace pu::tests
