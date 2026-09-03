// SPDX-License-Identifier: GPL-3.0-only
#include "mcp/http_transport.hpp"

#include "pu/infra/platform.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace pu::mcp {

HttpTransport::HttpTransport(std::string url,
                             std::map<std::string, std::string> headers)
    : url_(std::move(url)), headers_(std::move(headers)) {}

HttpTransport::~HttpTransport() { Stop(); }

bool HttpTransport::Start(MessageCallback on_message) {
  if (running_) return true;
  on_message_ = std::move(on_message);
  stopping_ = false;

  // Abort an in-flight transfer when Stop() is requested (or the global
  // interrupt flag is raised). The curl progress hook polls this callback.
  http_.SetInterruptChecker([this] {
    return stopping_.load(std::memory_order_acquire) ||
           pu::platform::IsInterrupted();
  });

  running_ = true;
  worker_ = std::thread(&HttpTransport::WorkerLoop, this);
  return true;
}

void HttpTransport::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    stopping_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool HttpTransport::WriteLine(const std::string& line) {
  if (!running_) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(line);
  }
  cv_.notify_one();
  return true;
}

std::vector<std::string> HttpTransport::BuildHeaders() const {
  std::vector<std::string> headers;
  headers.reserve(headers_.size() + 2);
  headers.push_back("Content-Type: application/json");
  headers.push_back("Accept: application/json, text/event-stream");
  for (const auto& [key, value] : headers_) {
    headers.push_back(key + ": " + value);
  }
  return headers;
}

void HttpTransport::DispatchChunk(const char* data, size_t size) {
  leftover_.append(data, size);
  size_t pos = 0;
  while ((pos = leftover_.find('\n')) != std::string::npos) {
    std::string line = leftover_.substr(0, pos);
    leftover_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    EmitLine(line);
  }
}

void HttpTransport::EmitLine(const std::string& line) {
  if (line.empty()) return;

  std::string payload;
  if (line.rfind("data:", 0) == 0) {
    // SSE framing: JSON-RPC messages arrive on "data:" lines.
    payload = line.substr(5);
    if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
    if (payload == "[DONE]") return;
  } else if (line.rfind("event:", 0) == 0 || line.rfind("id:", 0) == 0 ||
             line.rfind("retry:", 0) == 0) {
    // SSE metadata lines carry no JSON-RPC payload.
    return;
  } else {
    payload = line;
  }

  if (payload.empty()) return;
  if (on_message_) on_message_(payload);
}

void HttpTransport::WorkerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_) {
    cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
    if (!running_) break;

    std::string body = std::move(queue_.front());
    queue_.pop();
    lock.unlock();

    try {
      auto headers = BuildHeaders();
      http_.PostStream(url_, body, headers,
                       [this](char* data, size_t size) -> size_t {
                         DispatchChunk(data, size);
                         return size;
                       });
      // Flush any trailing data that arrived without a newline.
      if (!leftover_.empty()) {
        EmitLine(leftover_);
        leftover_.clear();
      }
    } catch (const std::exception& e) {
      if (!stopping_) {
        spdlog::error("MCP HTTP request to '{}' failed: {}", url_, e.what());
      }
    }

    lock.lock();
  }
}

}  // namespace pu::mcp
