// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "pu/agent_config.hpp"
#include "pu/infra/platform.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"

namespace pu::cli {

namespace {

constexpr const char* kWebDir = "./web";

nlohmann::json SessionInfoJson(const std::shared_ptr<Session>& session) {
  nlohmann::json j;
  if (!session) return j;
  const auto& spec = session->GetRuntimeSpec();
  j["agent_name"] = spec.agent_name;
  j["backend_type"] =
      spec.backend.type == config::BackendType::kOpenAI ? "openai" : "ollama";
  j["backend_model"] = spec.backend.model;
  j["backend_host"] = spec.backend.host;
  return j;
}

void SendJson(httplib::Response& res, int status, const nlohmann::json& j) {
  res.status = status;
  res.set_header("Content-Type", "application/json");
  res.body = j.dump();
}

}  // namespace

int RunServe(int argc, char* argv[], Runtime& runtime) {
  std::string host = "127.0.0.1";
  int port = 8080;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu serve [--host <host>] [--port <port>]\n"
                << "Options:\n"
                << "  --host <host>   Address to bind (default 127.0.0.1)\n"
                << "  --port <port>   TCP port to listen on (default 8080)\n"
                << "  -h, --help      Show this help message\n";
      return 0;
    } else if (arg == "--host") {
      if (i + 1 < argc) {
        host = argv[++i];
      } else {
        spdlog::error("--host requires an argument");
        return 1;
      }
    } else if (arg == "--port") {
      if (i + 1 >= argc) {
        spdlog::error("--port requires an argument");
        return 1;
      }
      std::string val = argv[++i];
      char* end = nullptr;
      long parsed = std::strtol(val.c_str(), &end, 10);
      if (!end || *end != '\0' || parsed < 1 || parsed > 65535) {
        spdlog::error("invalid port '{}'", val);
        return 1;
      }
      port = static_cast<int>(parsed);
    } else {
      spdlog::error("unexpected argument '{}'", arg);
      return 1;
    }
  }

  try {
    runtime.Initialize();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }

  // ProcessInput mutates the single persistent Session (workspace + history),
  // so all handlers that touch the runtime/session share one lock.
  std::mutex io_mutex;
  httplib::Server svr;

  // Static front-end. Mount points only serve GET/HEAD; specific API routes
  // registered below take precedence through regular handler dispatch.
  if (!svr.set_mount_point("/", kWebDir)) {
    spdlog::warn("Web root '{}' not found - serving API only", kWebDir);
  }

  // POST /api/chat - send a message to the active agent.
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

    ExecutionResult result;
    bool is_command = false;
    try {
      std::lock_guard<std::mutex> lock(io_mutex);
      bool ok = runtime.ProcessInput(message, result, is_command);
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

  // GET /api/session - current session info.
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

  // POST /api/clear - wipe conversation history and artifacts.
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

  // GET /api/agents - available agent list (placeholder for now).
  svr.Get("/api/agents", [](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j;
    j["agents"] = nlohmann::json::array();
    SendJson(res, 200, j);
  });

  spdlog::info("pu serve listening on http://{}:{} (web UI at /)", host, port);

  std::atomic<bool> listen_done{false};
  bool listen_ok = false;
  std::thread server_thread([&]() {
    listen_ok = svr.listen(host.c_str(), port);
    listen_done = true;
  });

  // main() installed a SIGINT handler that sets pu::platform's interrupt flag;
  // poll it so Ctrl+C stops the blocking listen and shuts down cleanly.
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
