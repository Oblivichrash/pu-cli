// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "infra/curl_http_client.hpp"
#include "pu/mcp/stdio_transport.hpp"

namespace pu::mcp {

// Remote streamable-HTTP MCP transport: WriteLine() POSTs a JSON-RPC body via
// CurlHttpClient on a worker thread; response lines (including SSE "data:"
// payloads) are forwarded to the MessageCallback. Stop() aborts in-flight
// requests through the curl progress hook.
class HttpTransport : public Transport {
 public:
  HttpTransport(std::string url, std::map<std::string, std::string> headers);
  ~HttpTransport() override;

  HttpTransport(const HttpTransport&) = delete;
  HttpTransport& operator=(const HttpTransport&) = delete;

  bool Start(MessageCallback on_message) override;
  void Stop() override;
  bool WriteLine(const std::string& line) override;

 private:
  void WorkerLoop();
  void DispatchChunk(const char* data, size_t size);
  void EmitLine(const std::string& line);
  std::vector<std::string> BuildHeaders() const;

  std::string url_;
  std::map<std::string, std::string> headers_;
  pu::http::CurlHttpClient http_;
  MessageCallback on_message_;

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};

  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::string> queue_;
  std::string leftover_;  // worker-thread only
};

}  // namespace pu::mcp
