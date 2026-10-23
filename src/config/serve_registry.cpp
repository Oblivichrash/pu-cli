// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/serve_registry.hpp"

#include "pu/core/json.hpp"
#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace pu::config {

namespace {

constexpr std::string_view kRegistryFileName = "serve.json";
constexpr std::string_view kRecordsKey = "servers";

// A record is written by one server and read by another process, so the file is replaced
// whole rather than edited: the rename is what makes a reader see either the old list or the
// new one, never a half-written line. Replacing whole is not enough on its own, though, because
// two servers started together would each read the list before the other had written, and the
// second write would drop the first server. The lock file makes the read and the write one
// step for everyone on the machine.
class RegistryLock {
 public:
  explicit RegistryLock(const std::string& path) : path_(path + ".lock") {
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (TryLock()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    spdlog::warn("The serve registry at {} stayed locked; writing anyway", path);
  }

  ~RegistryLock() {
    if (!held_) return;
#ifdef _WIN32
    CloseHandle(handle_);
#else
    ::close(fd_);
#endif
  }

  RegistryLock(const RegistryLock&) = delete;
  RegistryLock& operator=(const RegistryLock&) = delete;

  bool held() const { return held_; }

 private:
  bool TryLock() {
#ifdef _WIN32
    handle_ = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    held_ = handle_ != INVALID_HANDLE_VALUE;
    if (!held_) {
      const DWORD error = GetLastError();
      if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
        spdlog::warn("Cannot open the serve registry lock at {}: {}", path_, error);
        return true;
      }
    }
    return held_;
#else
    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) return true;
    held_ = ::flock(fd_, LOCK_EX | LOCK_NB) == 0;
    if (!held_) ::close(fd_);
    return held_;
#endif
  }

  std::string path_;
  bool held_ = false;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

// The temp file is named after the process that is writing it. The lock is a courtesy and not a
// guarantee — a caller that gave up waiting writes anyway — so two writers can be renaming at
// once, and a single shared `serve.json.tmp` would let the slower one publish a half-written file
// over the faster one's finished list.
std::string TempPathFor(const std::string& path) {
  return path + "." + std::to_string(platform::CurrentProcessId()) + ".tmp";
}

void WriteRecords(const std::string& path, const json::array& records) {
  if (path.empty()) return;
  const std::string text = json::serialize(json::value{json::object{{kRecordsKey, records}}});
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  const std::string temp = TempPathFor(path);
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      spdlog::warn("Cannot write the serve registry at {}", temp);
      return;
    }
    file << text;
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) spdlog::warn("Cannot replace the serve registry at {}: {}", path, ec.message());
}

json::array ReadRecords(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::fprintf(stderr, "PROBE unreadable path=%s\n", path.c_str());
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  std::fprintf(stderr, "PROBE read path=%s bytes=%zu\n", path.c_str(), buffer.str().size());
  json::value parsed;
  try {
    parsed = json::parse(buffer.str());
  } catch (const std::exception&) {
    spdlog::warn("The serve registry at {} is not readable as JSON", path);
    return {};
  }
  if (!json::HasKey(parsed, kRecordsKey)) {
    std::fprintf(stderr, "PROBE no key\n");
    return {};
  }
  const json::value& records = parsed.at(kRecordsKey);
  if (!records.is_array()) {
    std::fprintf(stderr, "PROBE not an array\n");
    return {};
  }
  std::fprintf(stderr, "PROBE records=%zu\n", records.as_array().size());
  return records.as_array();
}

ServeRecord RecordFromJson(const json::value& item) {
  ServeRecord record;
  record.directory = json::ValueOrDefault<std::string>(item, "directory", "");
  record.bind = json::ValueOrDefault<std::string>(item, "bind", "");
  record.port = static_cast<std::uint16_t>(json::ValueOrDefault<int>(item, "port", 0));
  record.pid = json::ValueOrDefault<int>(item, "pid", 0);
  record.started = json::ValueOrDefault<std::string>(item, "started", "");
  return record;
}

json::value RecordToJson(const ServeRecord& record) {
  return json::object{
      {"directory", record.directory},
      {"bind", record.bind},
      {"port", record.port},
      {"pid", record.pid},
      {"started", record.started},
  };
}

// The directory as a list of segments, with `.` and `..` resolved away. A server started from
// `..\pu-cli` or `./` serves a real directory under a name that says nothing about it, and taking
// the last two segments of the unnormalized text would label the workspace `.` or `..`.
std::vector<std::string> Segments(const std::string& directory) {
  std::vector<std::string> parts;
  for (const auto& part : std::filesystem::path(directory).lexically_normal()) {
    const std::string text = part.string();
    if (text.empty() || text == "/" || text == "." || text == "..") continue;
    // A root name such as `C:` is a segment on Windows and a workspace can be driven from it, so
    // it stays; only the separators and the two relative markers are dropped.
    parts.push_back(text);
  }
  return parts;
}

}  // namespace

std::string WorkspaceLabel(const std::string& directory) {
  const std::vector<std::string> parts = Segments(directory);
  if (parts.empty()) return directory;
  if (parts.size() == 1) return parts.front();
  return parts[parts.size() - 2] + "/" + parts.back();
}

std::string ServeRegistryPath() {
  // HOME is what the rest of the configuration layer reads, so it is read first; USERPROFILE is
  // the Windows answer for the same thing, and is where the file goes when HOME is not set. The
  // path is joined, not resolved: a home that does not exist yet is a valid answer, and resolving
  // would either throw or invent a different directory for the caller to write into.
  const char* home = std::getenv("HOME");
#if defined(_WIN32)
  if (home == nullptr || *home == '\0') home = std::getenv("USERPROFILE");
#endif
  if (home == nullptr || *home == '\0') return {};
  return (std::filesystem::path(home) / ".pu" / kRegistryFileName).string();
}

bool StillRunning(const ServeRecord& record) {
  const bool alive = platform::ProcessRunning(record.pid);
  const std::string now = platform::ProcessStartToken(record.pid);
  if (!alive) return false;
  if (record.started.empty() || now.empty()) return true;
  return record.started == now;
}

std::vector<ServeRecord> LiveServeRecords() {
  const std::string path = ServeRegistryPath();
  if (path.empty()) return {};

  RegistryLock lock(path);
  const json::array stored = ReadRecords(path);
  std::vector<ServeRecord> live;
  json::array kept;
  for (const json::value& item : stored) {
    ServeRecord record = RecordFromJson(item);
    if (!StillRunning(record)) {
      std::fprintf(stderr, "PROBE dropped pid=%d started=[%s] now=[%s]\n", record.pid,
                   record.started.c_str(),
                   platform::ProcessStartToken(record.pid).c_str());
      spdlog::debug("Dropping the serve record left by process {}", record.pid);
      continue;
    }
    kept.push_back(item);
    live.push_back(std::move(record));
  }
  if (kept.size() != stored.size()) WriteRecords(path, kept);
  std::stable_sort(live.begin(), live.end(), [](const ServeRecord& a, const ServeRecord& b) {
    return a.port > b.port;
  });
  return live;
}

std::vector<ServeRecord> OthersAmong(const std::vector<ServeRecord>& records, int pid) {
  std::vector<ServeRecord> others;
  for (const ServeRecord& record : records) {
    if (record.pid != pid) others.push_back(record);
  }
  return others;
}

std::string RegisterServeRecord(const ServeRecord& record) {
  const std::string path = ServeRegistryPath();
  if (path.empty()) {
    spdlog::warn("HOME is not set, so this server cannot be listed on the page");
    std::fprintf(stderr, "PROBE register home unset\n");
    return {};
  }
  RegistryLock lock(path);
  json::array records;
  for (const json::value& item : ReadRecords(path)) {
    const ServeRecord existing = RecordFromJson(item);
    if (existing.pid != record.pid && platform::ProcessRunning(existing.pid)) {
      records.push_back(item);
      continue;
    }
    std::fprintf(stderr, "PROBE register forgot pid=%d running=%d\n", existing.pid,
                 platform::ProcessRunning(existing.pid) ? 1 : 0);
  }
  records.push_back(RecordToJson(record));
  WriteRecords(path, records);
  std::fprintf(stderr, "PROBE register wrote %zu to %s\n", records.size(), path.c_str());
  return path;
}

void UnregisterServeRecord(int pid) {
  const std::string path = ServeRegistryPath();
  if (path.empty()) return;
  RegistryLock lock(path);
  json::array kept;
  for (const json::value& item : ReadRecords(path)) {
    if (RecordFromJson(item).pid != pid) kept.push_back(item);
  }
  WriteRecords(path, kept);
}

}  // namespace pu::config
