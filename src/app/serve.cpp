// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pu/agent_config.hpp"
#include "pu/executor.hpp"
#include "pu/infra/platform.hpp"
#include "pu/json.hpp"
#include "pu/runtime.hpp"
#include "pu/session/session.hpp"

namespace pu::cli {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

void SendJson(http::response<http::string_body>& res, unsigned status,
              const boost::json::value& jv) {
  res.result(status);
  res.set(http::field::content_type, "application/json");
  res.body() = boost::json::serialize(jv);
  res.prepare_payload();
}

std::string GetWebDir() {
  static std::string dir;
  static std::once_flag flag;
  std::call_once(flag, []() {
    const char* env = std::getenv("PU_WEB_DIR");
    if (env && *env) {
      dir = env;
      return;
    }
    std::vector<std::string> candidates = {
      "./web",
      "../share/pu/web",
      "/usr/share/pu/web",
      "/usr/local/share/pu/web",
    };
    for (const auto& d : candidates) {
      if (std::filesystem::exists(d) && std::filesystem::is_directory(d)) {
        dir = d;
        return;
      }
    }
    dir = "./web";
  });
  return dir;
}

void ServeFile(const std::string& target, http::response<http::string_body>& res) {
  std::string base = GetWebDir();
  std::string path = base + target;
  if (target == "/")
    path = base + "/index.html";

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    res.result(http::status::not_found);
    res.prepare_payload();
    return;
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  res.result(http::status::ok);
  res.body() = std::move(content);
  if (path.ends_with(".html"))
    res.set(http::field::content_type, "text/html");
  else if (path.ends_with(".css"))
    res.set(http::field::content_type, "text/css");
  else if (path.ends_with(".js"))
    res.set(http::field::content_type, "application/javascript");
  else
    res.set(http::field::content_type, "application/octet-stream");
  res.prepare_payload();
}

void HandleApiSession(Runtime& runtime, std::mutex& io_mutex,
                      http::request<http::string_body>&&,
                      http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::object{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto session = runtime.GetDefaultSession();
    jv.as_object()["ok"] = session != nullptr;
    if (session) {
      const auto& spec = session->GetRuntimeSpec();
      jv.as_object()["agent_name"] = spec.agent_name;
      jv.as_object()["backend_type"] =
          spec.backend.type == config::BackendType::kOpenAI ? "openai" : "ollama";
      jv.as_object()["backend_model"] = spec.backend.model;
      jv.as_object()["backend_host"] = spec.backend.host;
    } else {
      jv.as_object()["error"] = "No active session";
    }
  } catch (const std::exception& e) {
    jv.as_object()["ok"] = false;
    jv.as_object()["error"] = e.what();
  }
  SendJson(res, 200, jv);
}

void HandleApiHistory(Runtime& runtime, std::mutex& io_mutex,
                      http::request<http::string_body>&&,
                      http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::array{};
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
        if (!msg.tool_calls_json.empty())
          item.as_object()["tool_calls_json"] = msg.tool_calls_json;
        if (!msg.tool_call_id.empty())
          item.as_object()["tool_call_id"] = msg.tool_call_id;
        if (!msg.tool_name.empty())
          item.as_object()["tool_name"] = msg.tool_name;
        if (!msg.reasoning_content.empty())
          item.as_object()["reasoning_content"] = msg.reasoning_content;
        jv.as_array().push_back(item);
      }
    }
  } catch (...) {}
  SendJson(res, 200, jv);
}

void HandleApiAgents(Runtime& runtime, std::mutex& io_mutex,
                     http::request<http::string_body>&&,
                     http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::object{};
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
  jv.as_object()["agents"] = agents;
  SendJson(res, 200, jv);
}

void HandleApiAgentSwitch(Runtime& runtime, std::mutex& io_mutex,
                          http::request<http::string_body>&& req,
                          http::response<http::string_body>& res) {
  boost::json::value body;
  try {
    body = boost::json::parse(req.body());
  } catch (...) {
    boost::json::value err = {{"success", false}, {"error", "Invalid JSON"}};
    SendJson(res, 400, err);
    return;
  }
  if (!json::HasKey(body, "agent_name") || !body.at("agent_name").is_string()) {
    boost::json::value err = {{"success", false}, {"error", "Missing or invalid 'agent_name'"}};
    SendJson(res, 400, err);
    return;
  }
  std::string agent_name = boost::json::value_to<std::string>(body.at("agent_name"));
  boost::json::value resp = boost::json::object{};
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
}

void HandleApiClear(Runtime& runtime, std::mutex& io_mutex,
                    http::request<http::string_body>&&,
                    http::response<http::string_body>& res) {
  boost::json::value jv = boost::json::object{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto session = runtime.GetDefaultSession();
    if (session) {
      session->GetWorkspace().ClearHistory();
      session->GetWorkspace().ClearArtifacts();
      jv.as_object()["success"] = true;
    } else {
      jv.as_object()["success"] = false;
      jv.as_object()["error"] = "No active session";
    }
  } catch (const std::exception& e) {
    jv.as_object()["success"] = false;
    jv.as_object()["error"] = e.what();
  }
  SendJson(res, 200, jv);
}

void HandleApiWorkspaces(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&&,
                         http::response<http::string_body>& res) {
  boost::json::value resp = boost::json::object{};
  boost::json::array ws_array;
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    auto workspaces = runtime.ListWorkspaces();
    for (const auto& [name, path] : workspaces) {
      boost::json::value item = {
        {"name", name},
        {"path", path}
      };
      ws_array.push_back(item);
    }
  } catch (...) {}
  resp.as_object()["workspaces"] = ws_array;
  SendJson(res, 200, resp);
}

void HandleApiWorkspaceSwitch(Runtime& runtime, std::mutex& io_mutex,
                              http::request<http::string_body>&& req,
                              http::response<http::string_body>& res) {
  boost::json::value body;
  try {
    body = boost::json::parse(req.body());
  } catch (...) {
    boost::json::value err = {{"success", false}, {"error", "Invalid JSON"}};
    SendJson(res, 400, err);
    return;
  }
  if (!json::HasKey(body, "path") || !body.at("path").is_string()) {
    boost::json::value err = {{"success", false}, {"error", "Missing or invalid 'path'"}};
    SendJson(res, 400, err);
    return;
  }
  std::string path_str = boost::json::value_to<std::string>(body.at("path"));
  boost::json::value resp = boost::json::object{};
  try {
    std::lock_guard<std::mutex> lock(io_mutex);
    if (runtime.SwitchWorkspace(path_str)) {
      resp.as_object()["success"] = true;
      resp.as_object()["current"] = runtime.GetWorkspaceName();
    } else {
      resp.as_object()["success"] = false;
      resp.as_object()["error"] = "Switch failed";
    }
  } catch (const std::exception& e) {
    resp.as_object()["success"] = false;
    resp.as_object()["error"] = e.what();
  }
  SendJson(res, 200, resp);
}

struct ActiveWebSocket {
  std::unique_ptr<websocket::stream<tcp::socket>> ws;
  std::thread worker_thread;
  CancelToken cancel_token;
  std::atomic<bool> running{false};
  std::mutex mtx;
};

void DispatchHttpRequest(Runtime& runtime, std::mutex& io_mutex,
                         http::request<http::string_body>&& req,
                         http::response<http::string_body>& res) {
  auto target = req.target();

  if (target == "/" || target == "/index.html" || target == "/style.css" || target == "/app.js") {
    ServeFile(target, res);
    return;
  }

  if (target == "/api/session" && req.method() == http::verb::get) {
    HandleApiSession(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/history" && req.method() == http::verb::get) {
    HandleApiHistory(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/agents" && req.method() == http::verb::get) {
    HandleApiAgents(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/agent/switch" && req.method() == http::verb::post) {
    HandleApiAgentSwitch(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/clear" && req.method() == http::verb::post) {
    HandleApiClear(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/workspaces" && req.method() == http::verb::get) {
    HandleApiWorkspaces(runtime, io_mutex, std::move(req), res);
    return;
  }
  if (target == "/api/workspace/switch" && req.method() == http::verb::post) {
    HandleApiWorkspaceSwitch(runtime, io_mutex, std::move(req), res);
    return;
  }

  res.result(http::status::not_found);
  res.prepare_payload();
}

}  // namespace

int RunServe(const std::string& host, int port, Runtime& runtime) {
  try {
    runtime.Initialize();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }

  spdlog::info("pu serve listening on http://{}:{}", host, port);
  spdlog::info("WebSocket endpoint: ws://{}:{}/ws", host, port);
  spdlog::info("Workspace: {}", runtime.GetWorkspaceRoot().string());

  net::io_context ioc;
  tcp::acceptor acceptor(ioc, tcp::endpoint(net::ip::make_address(host), port));

  std::mutex io_mutex;
  auto active_ws = std::make_shared<ActiveWebSocket>();
  active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);

  std::function<void(beast::error_code, tcp::socket)> do_accept =
      [&](beast::error_code ec, tcp::socket socket) {
    if (ec) {
      if (ec != net::error::operation_aborted)
        spdlog::warn("Accept error: {}", ec.message());
      return;
    }

    std::thread([&, socket = std::move(socket)]() mutable {
      beast::flat_buffer buffer;
      http::request<http::string_body> req;
      beast::error_code ec;
      http::read(socket, buffer, req, ec);
      if (ec) {
        spdlog::warn("HTTP read error: {}", ec.message());
        return;
      }

      if (websocket::is_upgrade(req)) {
        if (active_ws->running) {
          active_ws->cancel_token->store(true);
          if (active_ws->worker_thread.joinable()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            active_ws->worker_thread.detach();
          }
          if (active_ws->ws && active_ws->ws->is_open()) {
            beast::error_code close_ec;
            active_ws->ws->close(websocket::close_code::normal, close_ec);
          }
          active_ws->running = false;
        }

        auto new_ws = std::make_unique<websocket::stream<tcp::socket>>(std::move(socket));
        new_ws->accept(req, ec);
        if (ec) {
          spdlog::warn("WebSocket accept error: {}", ec.message());
          return;
        }

        {
          std::lock_guard<std::mutex> lock(active_ws->mtx);
          active_ws->ws = std::move(new_ws);
          active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);
          active_ws->running = true;
        }

        active_ws->worker_thread = std::thread([&runtime, &io_mutex, active_ws]() {
          while (active_ws->running) {
            beast::flat_buffer buffer;
            std::string text;
            beast::error_code ec;
            active_ws->ws->read(buffer, ec);
            if (ec) {
              if (ec != websocket::error::closed)
                spdlog::warn("WebSocket read error: {}", ec.message());
              active_ws->cancel_token->store(true);
              active_ws->running = false;
              break;
            }
            text = beast::buffers_to_string(buffer.data());

            boost::json::value jv;
            try {
              jv = boost::json::parse(text);
            } catch (...) {
              continue;
            }
            if (!jv.is_object())
              continue;

            std::string type = json::ValueOrDefault<std::string>(jv, "type", "");
            if (type == "cancel") {
              active_ws->cancel_token->store(true);
              continue;
            } else if (type == "run") {
              active_ws->cancel_token->store(true);
              active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);
              auto token = active_ws->cancel_token;

              std::string payload_text = json::ValueOrDefault<std::string>(
                  json::ValueOrDefault<boost::json::value>(jv, "payload", boost::json::object{}),
                  "text", "");
              if (payload_text.empty())
                continue;

              std::thread worker([&runtime, &io_mutex, active_ws, token, payload_text]() {
                bool is_command = false;
                ExecutionResult result;

                // Serialize and send one JSON frame to the WebSocket client.
                auto send_frame = [&](const boost::json::value& ev) {
                  std::string msg = boost::json::serialize(ev);
                  beast::error_code ec;
                  std::lock_guard<std::mutex> ws_lock(active_ws->mtx);
                  if (active_ws->ws && active_ws->ws->is_open())
                    active_ws->ws->write(net::buffer(msg), ec);
                };

                // Tool call lifecycle callbacks: forward start/end events so the
                // front-end can display tool invocations in real time.
                Executor::ToolCallbacks tool_cb;
                tool_cb.on_start = [&](const std::string& id,
                                       const std::string& name,
                                       const boost::json::value& args) {
                  boost::json::value frame = {
                      {"type", "tool_start"},
                      {"payload", {{"id", id}, {"name", name}, {"args", args}}}};
                  send_frame(frame);
                };
                tool_cb.on_end = [&](const std::string& id,
                                     const std::string& output,
                                     const std::string& error) {
                  boost::json::value frame = {
                      {"type", "tool_end"},
                      {"payload", {{"id", id}, {"output", output}, {"error", error}}}};
                  send_frame(frame);
                };

                {
                  std::lock_guard<std::mutex> lock(io_mutex);
                  result = runtime.ProcessInput(
                      payload_text, is_command, token,
                      [&](const std::string& chunk) {
                        if (chunk.empty())
                          return;
                        boost::json::value ev = {{"type", "chunk"}, {"payload", {{"text", chunk}}}};
                        send_frame(ev);
                      },
                      tool_cb);
                }

                boost::json::value final;
                if (result.has_error)
                  final = {{"type", "error"}, {"payload", {{"text", result.error_message}}}};
                else
                  final = {{"type", "done"}};
                send_frame(final);
              });
              worker.detach();
            }
          }
        });
        active_ws->worker_thread.detach();

      } else {
        http::response<http::string_body> res;
        DispatchHttpRequest(runtime, io_mutex, std::move(req), res);
        http::write(socket, res, ec);
        if (ec)
          spdlog::warn("HTTP write error: {}", ec.message());
      }
    }).detach();

    acceptor.async_accept(ioc, do_accept);
  };

  acceptor.async_accept(ioc, do_accept);

  std::thread ioc_thread([&]() { ioc.run(); });

  while (!pu::platform::IsInterrupted())
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

  ioc.stop();
  if (ioc_thread.joinable())
    ioc_thread.join();

  if (active_ws->running) {
    active_ws->cancel_token->store(true);
    if (active_ws->worker_thread.joinable()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      active_ws->worker_thread.detach();
    }
    if (active_ws->ws && active_ws->ws->is_open()) {
      beast::error_code ec;
      active_ws->ws->close(websocket::close_code::normal, ec);
    }
  }

  runtime.Shutdown();
  return 0;
}

}  // namespace pu::cli
