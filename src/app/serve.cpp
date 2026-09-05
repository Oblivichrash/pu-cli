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
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include "pu/agent_config.hpp"
#include "pu/infra/platform.hpp"
#include "pu/json.hpp"
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

boost::json::value SessionInfoJson(const std::shared_ptr<Session>& session) {
  boost::json::value j = boost::json::object{};
  if (!session) return j;
  const auto& spec = session->GetRuntimeSpec();
  j.as_object()["agent_name"] = spec.agent_name;
  j.as_object()["backend_type"] =
      spec.backend.type == config::BackendType::kOpenAI ? "openai" : "ollama";
  j.as_object()["backend_model"] = spec.backend.model;
  j.as_object()["backend_host"] = spec.backend.host;
  return j;
}

void SendJson(httplib::Response& res, int status, const boost::json::value& j) {
  res.status = status;
  res.set_header("Content-Type", "application/json");
  res.body = boost::json::serialize(j);
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
    boost::json::value resp = boost::json::object{};
    boost::json::value body;
    try {
      body = boost::json::parse(req.body);
    } catch (const std::exception&) {
      resp.as_object()["success"] = false;
      resp.as_object()["content"] = "";
      resp.as_object()["error"] = "Invalid JSON body";
      SendJson(res, 400, resp);
      return;
    }
    if (!json::HasKey(body, "message") || !body.at("message").is_string()) {
      resp.as_object()["success"] = false;
      resp.as_object()["content"] = "";
      resp.as_object()["error"] = "Missing or invalid 'message' field";
      SendJson(res, 400, resp);
      return;
    }
    const std::string message =
        boost::json::value_to<std::string>(body.at("message"));
    if (message.empty()) {
      resp.as_object()["success"] = false;
      resp.as_object()["content"] = "";
      resp.as_object()["error"] = "'message' must not be empty";
      SendJson(res, 400, resp);
      return;
    }

    std::string request_id;
    if (json::HasKey(body, "request_id") && body.at("request_id").is_string() &&
        !boost::json::value_to<std::string>(body.at("request_id")).empty()) {
      request_id = boost::json::value_to<std::string>(body.at("request_id"));
    } else {
      static std::atomic<uint64_t> s_request_seq{0};
      request_id =
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(s_request_seq.fetch_add(1));
    }
    resp.as_object()["request_id"] = request_id;

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
      resp.as_object()["success"] = ok && !result.has_error;
      resp.as_object()["content"] = result.content;
      resp.as_object()["error"] = result.has_error ? result.error_message : "";
      resp.as_object()["is_command"] = is_command;
      resp.as_object()["tool_call_count"] = result.tool_call_count;
      if (!ok && result.error_message.empty())
        resp.as_object()["error"] = "Processing failed";
    } catch (const std::exception& e) {
      resp.as_object()["success"] = false;
      resp.as_object()["content"] = "";
      resp.as_object()["error"] = e.what();
    }
    SendJson(res, 200, resp);
  });

  svr.Post("/api/chat/stream", [&](const httplib::Request& req,
                                   httplib::Response& res) {
    boost::json::value resp = boost::json::object{};
    boost::json::value body;
    try {
      body = boost::json::parse(req.body);
    } catch (const std::exception&) {
      resp.as_object()["success"] = false;
      resp.as_object()["content"] = "";
      resp.as_object()["error"] = "Invalid JSON body";
      SendJson(res, 400, resp);
      return;
    }
    if (!json::HasKey(body, "message") || !body.at("message").is_string()) {
      resp.as_object()["success"] = false;
      resp.as_object()["content"] = "";
      resp.as_object()["error"] = "Missing or invalid 'message' field";
      SendJson(res, 400, resp);
      return;
    }
    const std::string message =
        boost::json::value_to<std::string>(body.at("message"));
    if (message.empty()) {
      resp.as_object()["success"] = false;
      resp.as_object()["content"] = "";
      resp.as_object()["error"] = "'message' must not be empty";
      SendJson(res, 400, resp);
      return;
    }

    std::string request_id;
    if (json::HasKey(body, "request_id") && body.at("request_id").is_string() &&
        !boost::json::value_to<std::string>(body.at("request_id")).empty()) {
      request_id = boost::json::value_to<std::string>(body.at("request_id"));
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
              boost::json::value ev = {{"token", chunk}};
              PushSseEvent(sse, "data: " + boost::json::serialize(ev) + "\n\n");
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
        boost::json::value ev = {
          {"error",
           result.has_error && !result.error_message.empty()
               ? result.error_message
               : "Processing failed"}};
        PushSseEvent(sse, "data: " + boost::json::serialize(ev) + "\n\n");
      } else if (!streamed && !result.content.empty()) {
        boost::json::value ev = {{"token", result.content}};
        PushSseEvent(sse, "data: " + boost::json::serialize(ev) + "\n\n");
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
    boost::json::value resp = boost::json::object{};
    boost::json::value body;
    try {
      body = boost::json::parse(req.body);
    } catch (const std::exception&) {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = "Invalid JSON body";
      SendJson(res, 400, resp);
      return;
    }
    if (!json::HasKey(body, "request_id") || !body.at("request_id").is_string() ||
        boost::json::value_to<std::string>(body.at("request_id")).empty()) {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = "Missing or invalid 'request_id' field";
      SendJson(res, 400, resp);
      return;
    }
    const std::string request_id =
        boost::json::value_to<std::string>(body.at("request_id"));
    CancelToken token;
    {
      std::lock_guard<std::mutex> lock(cancel_mutex);
      auto found = active_requests.find(request_id);
      if (found == active_requests.end()) {
        resp.as_object()["success"] = false;
        resp.as_object()["error"] = "request not found";
        SendJson(res, 404, resp);
        return;
      }
      token = found->second;
    }
    if (token) {
      token->store(true);
      spdlog::info("Cancel requested for request '{}'", request_id);
    }
    resp.as_object()["success"] = true;
    SendJson(res, 200, resp);
  });

  svr.Get("/api/session", [&](const httplib::Request&, httplib::Response& res) {
    boost::json::value j = boost::json::object{};
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto session = runtime.GetDefaultSession();
      j.as_object()["ok"] = session != nullptr;
      if (session) {
        json::Merge(j, SessionInfoJson(session));
      } else {
        j.as_object()["error"] = "No active session";
      }
    } catch (const std::exception& e) {
      j.as_object()["ok"] = false;
      j.as_object()["error"] = e.what();
    }
    SendJson(res, 200, j);
  });

  svr.Get("/api/history", [&](const httplib::Request&, httplib::Response& res) {
    boost::json::value j = boost::json::array{};
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto session = runtime.GetDefaultSession();
      if (session) {
        auto history = session->GetWorkspace().GetHistory();
        for (const auto& msg : history) {
          boost::json::value item = {
            {"id", msg.id},
            {"role", msg.role},
            {"content", msg.content},
            {"timestamp", msg.timestamp},
          };
          if (!msg.tool_calls_json.empty()) {
            item.as_object()["tool_calls_json"] = msg.tool_calls_json;
          }
          if (!msg.tool_call_id.empty()) {
            item.as_object()["tool_call_id"] = msg.tool_call_id;
          }
          if (!msg.tool_name.empty()) {
            item.as_object()["tool_name"] = msg.tool_name;
          }
          if (!msg.reasoning_content.empty()) {
            item.as_object()["reasoning_content"] = msg.reasoning_content;
          }
          j.as_array().push_back(item);
        }
      }
    } catch (const std::exception&) {
    }
    SendJson(res, 200, j);
  });

  svr.Get("/api/agents", [&](const httplib::Request&, httplib::Response& res) {
    boost::json::value j = boost::json::object{};
    boost::json::array agents;
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto& mgr = runtime.GetAgentManager();
      auto names = mgr.GetAgentNames();
      for (const auto& name : names) {
        auto* cfg = mgr.GetAgentConfig(name);
        boost::json::value item = {
          {"name", name},
          {"description", cfg ? cfg->description : ""},
        };
        agents.push_back(item);
      }
    } catch (...) {}
    j.as_object()["agents"] = agents;
    SendJson(res, 200, j);
  });

  svr.Post("/api/agent/switch", [&](const httplib::Request& req, httplib::Response& res) {
    boost::json::value resp = boost::json::object{};
    boost::json::value body;
    try {
      body = boost::json::parse(req.body);
    } catch (...) {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = "Invalid JSON";
      SendJson(res, 400, resp);
      return;
    }
    if (!json::HasKey(body, "agent_name") || !body.at("agent_name").is_string()) {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = "Missing or invalid 'agent_name'";
      SendJson(res, 400, resp);
      return;
    }
    std::string agent_name =
        boost::json::value_to<std::string>(body.at("agent_name"));
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto& mgr = runtime.GetAgentManager();
      auto* cfg = mgr.GetAgentConfig(agent_name);
      if (!cfg) {
        resp.as_object()["success"] = false;
        resp.as_object()["error"] = "Agent not found: " + agent_name;
        SendJson(res, 404, resp);
        return;
      }
      runtime.SwitchAgent(*cfg);
      resp.as_object()["success"] = true;
      resp.as_object()["agent"] = agent_name;
    } catch (const std::exception& e) {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = e.what();
    }
    SendJson(res, 200, resp);
  });

  svr.Post("/api/clear", [&](const httplib::Request&, httplib::Response& res) {
    boost::json::value j = boost::json::object{};
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto session = runtime.GetDefaultSession();
      if (session) {
        session->GetWorkspace().ClearHistory();
        session->GetWorkspace().ClearArtifacts();
        j.as_object()["success"] = true;
      } else {
        j.as_object()["success"] = false;
        j.as_object()["error"] = "No active session";
      }
    } catch (const std::exception& e) {
      j.as_object()["success"] = false;
      j.as_object()["error"] = e.what();
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
