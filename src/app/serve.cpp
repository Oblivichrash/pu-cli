// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <libgen.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "pu/agent_config.hpp"
#include "pu/infra/platform.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"

namespace pu::cli {

namespace {

std::string GetExecutableDir() {
#ifdef _WIN32
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return ".";
  const std::string path(buf, n);
  const std::string::size_type pos = path.find_last_of("\\/");
  return pos == std::string::npos ? "." : path.substr(0, pos);
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n < 0) return ".";
  buf[n] = '\0';
  const std::string dir = dirname(buf);
  return dir.empty() ? "." : dir;
#endif
}

std::vector<std::string> GetWebDirCandidates() {
  std::vector<std::string> dirs;
  if (const char* env = std::getenv("PU_WEB_DIR"); env && *env != '\0') {
    dirs.emplace_back(env);
  }
  const std::string exe_dir = GetExecutableDir();
  dirs.push_back(exe_dir + "/../share/pu/web");
  dirs.push_back(exe_dir + "/../web");
  dirs.push_back("./web");
  dirs.push_back("/usr/share/pu/web");
  dirs.push_back("/usr/local/share/pu/web");
  return dirs;
}

nlohmann::json SessionInfoJson(const std::shared_ptr<Session>& session) {
  nlohmann::json j;
  if (!session) return j;
  const auto& spec = session->GetRuntimeSpec();
  j["agent_name"] = spec.agent_name;
  j["backend_type"] = spec.backend.type == config::BackendType::kOpenAI ? "openai" : "ollama";
  j["backend_model"] = spec.backend.model;
  j["backend_host"] = spec.backend.host;
  return j;
}

void SendJson(httplib::Response& res, int status, const nlohmann::json& j) {
  res.status = status;
  res.set_header("Content-Type", "application/json");
  res.body = j.dump();
}

struct SseStream {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::string> events;
  bool finished = false;
};

void PushSseEvent(const std::shared_ptr<SseStream>& sse, std::string event) {
  {
    std::lock_guard<std::mutex> lock(sse->mutex);
    sse->events.push_back(std::move(event));
  }
  sse->cv.notify_one();
}

class ActiveRequestGuard {
 public:
  ActiveRequestGuard(std::mutex& mutex,
                     std::unordered_map<std::string, CancelToken>& requests,
                     std::string request_id)
      : mutex_(&mutex), requests_(&requests), request_id_(std::move(request_id)) {}
  ~ActiveRequestGuard() {
    if (!mutex_ || !requests_ || request_id_.empty()) return;
    std::lock_guard<std::mutex> lock(*mutex_);
    requests_->erase(request_id_);
  }
  ActiveRequestGuard(const ActiveRequestGuard&) = delete;
  ActiveRequestGuard& operator=(const ActiveRequestGuard&) = delete;

 private:
  std::mutex* mutex_;
  std::unordered_map<std::string, CancelToken>* requests_;
  std::string request_id_;
};

}  // namespace

int RunServe(const std::string& host, int port, Runtime& runtime) {
  try {
    runtime.Initialize();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }

  std::mutex io_mutex;
  std::mutex cancel_mutex;
  std::unordered_map<std::string, CancelToken> active_requests;
  httplib::Server svr;

  bool mounted = false;
  for (const auto& dir : GetWebDirCandidates()) {
    if (svr.set_mount_point("/", dir.c_str())) {
      spdlog::info("Web UI mounted at {}", dir);
      mounted = true;
      break;
    }
  }
  if (!mounted) {
    spdlog::warn("No web directory found; serving API only");
  }

  svr.Post("/api/chat", [&](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json resp;
    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
      resp["success"] = false;
      resp["content"] = "";
      resp["error"] = "Invalid JSON body";
      SendJson(res, 400, resp);
      return;
    }
    auto it = body.find("message");
    if (it == body.end() || !it->is_string()) {
      resp["success"] = false;
      resp["content"] = "";
      resp["error"] = "Missing or invalid 'message' field";
      SendJson(res, 400, resp);
      return;
    }
    const std::string message = it->get<std::string>();
    if (message.empty()) {
      resp["success"] = false;
      resp["content"] = "";
      resp["error"] = "'message' must not be empty";
      SendJson(res, 400, resp);
      return;
    }

    std::string request_id;
    auto rid_it = body.find("request_id");
    if (rid_it != body.end() && rid_it->is_string() &&
        !rid_it->get<std::string>().empty()) {
      request_id = rid_it->get<std::string>();
    } else {
      static std::atomic<uint64_t> s_request_seq{0};
      request_id =
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(s_request_seq.fetch_add(1));
    }
    resp["request_id"] = request_id;

    auto token = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> lock(cancel_mutex);
      active_requests[request_id] = token;
    }
    ActiveRequestGuard cleanup(cancel_mutex, active_requests, request_id);

    ExecutionResult result;
    bool is_command = false;
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      bool ok = runtime.ProcessInput(message, result, is_command, token);
      resp["success"] = ok && !result.has_error;
      resp["content"] = result.content;
      resp["error"] = result.has_error ? result.error_message : "";
      resp["is_command"] = is_command;
      resp["tool_call_count"] = result.tool_call_count;
      if (!ok && result.error_message.empty()) resp["error"] = "Processing failed";
    } catch (const std::exception& e) {
      resp["success"] = false;
      resp["content"] = "";
      resp["error"] = e.what();
    }
    SendJson(res, 200, resp);
  });

  svr.Post("/api/chat/stream", [&](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json resp;
    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
      resp["success"] = false;
      resp["content"] = "";
      resp["error"] = "Invalid JSON body";
      SendJson(res, 400, resp);
      return;
    }
    auto it = body.find("message");
    if (it == body.end() || !it->is_string()) {
      resp["success"] = false;
      resp["content"] = "";
      resp["error"] = "Missing or invalid 'message' field";
      SendJson(res, 400, resp);
      return;
    }
    const std::string message = it->get<std::string>();
    if (message.empty()) {
      resp["success"] = false;
      resp["content"] = "";
      resp["error"] = "'message' must not be empty";
      SendJson(res, 400, resp);
      return;
    }

    std::string request_id;
    auto rid_it = body.find("request_id");
    if (rid_it != body.end() && rid_it->is_string() &&
        !rid_it->get<std::string>().empty()) {
      request_id = rid_it->get<std::string>();
    } else {
      static std::atomic<uint64_t> s_request_seq{0};
      request_id =
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(s_request_seq.fetch_add(1));
    }
    auto token = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> lock(cancel_mutex);
      active_requests[request_id] = token;
    }

    auto sse = std::make_shared<SseStream>();

    std::thread worker([&, sse, token, request_id, message]() {
      ExecutionResult result;
      bool is_command = false;
      bool ok = false;
      bool streamed = false;
      try {
        std::lock_guard<std::mutex> lock(io_mutex);
        ok = runtime.ProcessInput(
            message, result, is_command, token,
            [&](const std::string& chunk) {
              if (chunk.empty()) return;
              streamed = true;
              nlohmann::json ev;
              ev["token"] = chunk;
              PushSseEvent(sse, "data: " + ev.dump() + "\n\n");
            });
      } catch (const std::exception& e) {
        result.has_error = true;
        result.error_message = e.what();
        ok = false;
      }

      {
        std::lock_guard<std::mutex> lock(cancel_mutex);
        active_requests.erase(request_id);
      }

      if (!ok || result.has_error) {
        nlohmann::json ev;
        ev["error"] = result.has_error && !result.error_message.empty()
                          ? result.error_message
                          : "Processing failed";
        PushSseEvent(sse, "data: " + ev.dump() + "\n\n");
      } else if (!streamed && !result.content.empty()) {
        nlohmann::json ev;
        ev["token"] = result.content;
        PushSseEvent(sse, "data: " + ev.dump() + "\n\n");
      }
      PushSseEvent(sse, "data: [DONE]\n\n");
      {
        std::lock_guard<std::mutex> lock(sse->mutex);
        sse->finished = true;
      }
      sse->cv.notify_all();
    });
    worker.detach();

    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider(
        "text/event-stream",
        [sse, token](size_t /*offset*/, httplib::DataSink& sink) -> bool {
          std::string event;
          {
            std::unique_lock<std::mutex> lock(sse->mutex);
            sse->cv.wait_for(lock, std::chrono::milliseconds(250), [&] {
              return !sse->events.empty() || sse->finished || !sink.is_writable();
            });
            if (!sse->events.empty()) {
              event = std::move(sse->events.front());
              sse->events.pop_front();
            } else if (sse->finished) {
              sink.done();
              return true;
            } else if (!sink.is_writable()) {
              token->store(true);
              return false;
            } else {
              return true;
            }
          }
          if (!sink.write(event.data(), event.size())) {
            token->store(true);
            return false;
          }
          return true;
        });
  });

  svr.Post("/api/chat/cancel", [&](const httplib::Request& req,
                                   httplib::Response& res) {
    nlohmann::json resp;
    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
      resp["success"] = false;
      resp["error"] = "Invalid JSON body";
      SendJson(res, 400, resp);
      return;
    }
    auto it = body.find("request_id");
    if (it == body.end() || !it->is_string() || it->get<std::string>().empty()) {
      resp["success"] = false;
      resp["error"] = "Missing or invalid 'request_id' field";
      SendJson(res, 400, resp);
      return;
    }
    const std::string request_id = it->get<std::string>();
    CancelToken token;
    {
      std::lock_guard<std::mutex> lock(cancel_mutex);
      auto found = active_requests.find(request_id);
      if (found == active_requests.end()) {
        resp["success"] = false;
        resp["error"] = "request not found";
        SendJson(res, 404, resp);
        return;
      }
      token = found->second;
    }
    if (token) {
      token->store(true);
      spdlog::info("Cancel requested for request '{}'", request_id);
    }
    resp["success"] = true;
    SendJson(res, 200, resp);
  });

  svr.Get("/api/session", [&](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j;
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto session = runtime.GetDefaultSession();
      j["ok"] = session != nullptr;
      if (session) {
        j.update(SessionInfoJson(session));
      } else {
        j["error"] = "No active session";
      }
    } catch (const std::exception& e) {
      j["ok"] = false;
      j["error"] = e.what();
    }
    SendJson(res, 200, j);
  });

  svr.Get("/api/history", [&](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j = nlohmann::json::array();
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto session = runtime.GetDefaultSession();
      if (session) {
        auto history = session->GetWorkspace().GetHistory();
        for (const auto& msg : history) {
          nlohmann::json item;
          item["id"] = msg.id;
          item["role"] = msg.role;
          item["content"] = msg.content;
          item["timestamp"] = msg.timestamp;
          if (!msg.tool_calls_json.empty()) {
            item["tool_calls_json"] = msg.tool_calls_json;
          }
          if (!msg.tool_call_id.empty()) {
            item["tool_call_id"] = msg.tool_call_id;
          }
          if (!msg.tool_name.empty()) {
            item["tool_name"] = msg.tool_name;
          }
          if (!msg.reasoning_content.empty()) {
            item["reasoning_content"] = msg.reasoning_content;
          }
          j.push_back(item);
        }
      }
    } catch (const std::exception&) {
    }
    SendJson(res, 200, j);
  });

  svr.Get("/api/agents", [&](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j;
    auto agents = nlohmann::json::array();
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto& mgr = runtime.GetAgentManager();
      auto names = mgr.GetAgentNames();
      for (const auto& name : names) {
        auto* cfg = mgr.GetAgentConfig(name);
        nlohmann::json item;
        item["name"] = name;
        item["description"] = cfg ? cfg->description : "";
        agents.push_back(item);
      }
    } catch (...) {}
    j["agents"] = agents;
    SendJson(res, 200, j);
  });

  svr.Post("/api/agent/switch", [&](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json resp;
    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (...) {
      resp["success"] = false;
      resp["error"] = "Invalid JSON";
      SendJson(res, 400, resp);
      return;
    }
    auto it = body.find("agent_name");
    if (it == body.end() || !it->is_string()) {
      resp["success"] = false;
      resp["error"] = "Missing or invalid 'agent_name'";
      SendJson(res, 400, resp);
      return;
    }
    std::string agent_name = it->get<std::string>();
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto& mgr = runtime.GetAgentManager();
      auto* cfg = mgr.GetAgentConfig(agent_name);
      if (!cfg) {
        resp["success"] = false;
        resp["error"] = "Agent not found: " + agent_name;
        SendJson(res, 404, resp);
        return;
      }
      runtime.SwitchAgent(*cfg);
      resp["success"] = true;
      resp["agent"] = agent_name;
    } catch (const std::exception& e) {
      resp["success"] = false;
      resp["error"] = e.what();
    }
    SendJson(res, 200, resp);
  });

  svr.Post("/api/clear", [&](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j;
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto session = runtime.GetDefaultSession();
      if (session) {
        session->GetWorkspace().ClearHistory();
        session->GetWorkspace().ClearArtifacts();
        j["success"] = true;
      } else {
        j["success"] = false;
        j["error"] = "No active session";
      }
    } catch (const std::exception& e) {
      j["success"] = false;
      j["error"] = e.what();
    }
    SendJson(res, 200, j);
  });

  spdlog::info("pu serve listening on http://{}:{} (web UI at /)", host, port);

  std::atomic<bool> listen_done{false};
  bool listen_ok = false;
  std::thread server_thread([&]() {
    listen_ok = svr.listen(host.c_str(), port);
    listen_done = true;
  });

  while (!listen_done && !pu::platform::IsInterrupted()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (!listen_done) {
    spdlog::info("Stopping HTTP server...");
    svr.stop();
  }
  server_thread.join();

  runtime.Shutdown();

  if (pu::platform::IsInterrupted()) {
    std::cout << "\nGoodbye!\n";
    return 0;
  }
  if (!listen_ok) {
    spdlog::error("Failed to listen on {}:{}", host, port);
    return 1;
  }
  return 0;
}

}  // namespace pu::cli
