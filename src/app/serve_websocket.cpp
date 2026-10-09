// SPDX-License-Identifier: GPL-3.0-only
#include "serve_internal.hpp"

#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "pu/core/json.hpp"
#include "pu/executor.hpp"
#include "pu/runtime.hpp"

namespace pu::cli::detail {

namespace beast = boost::beast;
namespace websocket = beast::websocket;

void RunWebSocketSession(tcp::socket socket, http::request<http::string_body> req, Runtime& runtime,
                         std::mutex& io_mutex, std::shared_ptr<ActiveWebSocket> active_ws) {
  // This session's own pointer to its own socket. It is deliberately not read back
  // out of `active_ws`: a session that has been replaced is still winding down, and
  // it must go on reading its own stream rather than the one that replaced it.
  auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));

  beast::error_code ec;
  ws->accept(req, ec);
  if (ec) {
    spdlog::warn("WebSocket accept error: {}", ec.message());
    return;
  }

  // Take the place of the client that was listening, if any. What that client was
  // watching is left alone: a reload is a reader coming back to the reply, so the
  // turn keeps its head down and the frames it has produced are handed over below.
  std::shared_ptr<websocket::stream<tcp::socket>> replaced;
  std::string handover;
  {
    std::lock_guard<std::mutex> lock(active_ws->mtx);
    replaced = std::exchange(active_ws->client, ws);
    if (active_ws->turn_in_flight) {
      boost::json::value resume = {{"type", "resume"},
                                   {"payload", {{"frames", active_ws->transcript}}}};
      handover = boost::json::serialize(resume);
    }
  }
  if (replaced) {
    // Closing the socket under its reader is how a blocking read is woken. The
    // stream object itself stays alive in that session's own pointer, so nothing is
    // freed under a thread still using it.
    beast::error_code close_ec;
    beast::get_lowest_layer(*replaced).close(close_ec);
  }
  // The handover goes out before anything else can be written, so the client is
  // never shown a fragment that arrived ahead of its own beginning.
  if (!handover.empty()) {
    std::lock_guard<std::mutex> lock(active_ws->mtx);
    if (active_ws->client == ws && ws->is_open()) {
      beast::error_code write_ec;
      ws->write(net::buffer(handover), write_ec);
    }
  }

  for (;;) {
    beast::flat_buffer buffer;
    beast::error_code read_ec;
    ws->read(buffer, read_ec);
    if (read_ec) {
      if (read_ec != websocket::error::closed) {
        spdlog::warn("WebSocket read error: {}", read_ec.message());
      }
      break;
    }

    const std::string text = beast::buffers_to_string(buffer.data());

    boost::json::value jv;
    try {
      jv = boost::json::parse(text);
    } catch (const std::exception& e) {
      boost::json::value error = {
          {"type", "error"}, {"payload", {{"text", std::string("Invalid JSON: ") + e.what()}}}};
      const std::string message = boost::json::serialize(error);
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      if (active_ws->client == ws && ws->is_open()) {
        beast::error_code write_ec;
        ws->write(net::buffer(message), write_ec);
      }
      continue;
    }
    if (!jv.is_object()) continue;

    const std::string type = json::ValueOrDefault<std::string>(jv, "type", "");
    if (type == "cancel") {
      // A stop the reader asked for. The turn ends where it is and keeps no answer,
      // which is what the client that asked is shown.
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      active_ws->cancel_token->store(true);
      continue;
    }
    if (type != "run") continue;

    const std::string payload_text = json::ValueOrDefault<std::string>(
        json::ValueOrDefault<boost::json::value>(jv, "payload", boost::json::object{}), "text", "");
    if (payload_text.empty()) continue;

    CancelToken token;
    {
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      // A turn still running is withdrawn first: there is one conversation here, so
      // a second request replaces the first rather than joining it.
      active_ws->cancel_token->store(true);
      active_ws->cancel_token = std::make_shared<std::atomic<bool>>(false);
      token = active_ws->cancel_token;
      active_ws->transcript.clear();
      active_ws->turn_in_flight = true;
    }

    std::thread([&runtime, &io_mutex, active_ws, token, payload_text]() {
      bool is_command = false;
      ExecutionResult result;

      // Says something on this turn's behalf: sent to the client that is listening
      // and recorded for the one that is not there yet. A turn that has been
      // replaced says nothing at all — its remarks belong to a request that was
      // withdrawn, and the transcript it would write into is the next turn's.
      const auto say = [&](const boost::json::value& frame) {
        const std::string message = boost::json::serialize(frame);
        std::lock_guard<std::mutex> lock(active_ws->mtx);
        if (active_ws->cancel_token != token) return;
        active_ws->transcript.push_back(frame);
        if (active_ws->client && active_ws->client->is_open()) {
          beast::error_code write_ec;
          active_ws->client->write(net::buffer(message), write_ec);
        }
      };

      // Tool call lifecycle callbacks: forward start/end events so the
      // front-end can display tool invocations in real time.
      ToolCallbacks tool_cb;
      tool_cb.on_start = [&](const std::string& id, const std::string& name,
                             const boost::json::value& args) {
        boost::json::value frame = {{"type", "tool_start"},
                                    {"payload", {{"id", id}, {"name", name}, {"args", args}}}};
        say(frame);
      };
      tool_cb.on_end = [&](const std::string& id, const std::string& output,
                           const std::string& error) {
        boost::json::value frame = {
            {"type", "tool_end"}, {"payload", {{"id", id}, {"output", output}, {"error", error}}}};
        say(frame);
      };

      {
        std::lock_guard<std::mutex> lock(io_mutex);
        result = runtime.ProcessInput(
            payload_text, is_command, token,
            [&](const std::string& chunk) {
              if (chunk.empty()) return;
              boost::json::value frame = {{"type", "chunk"}, {"payload", {{"text", chunk}}}};
              say(frame);
            },
            tool_cb,
            // Reasoning arrives on its own channel and is rendered beside the
            // answer, so it travels as a frame of its own.
            [&](const std::string& thought) {
              if (thought.empty()) return;
              boost::json::value frame = {{"type", "thinking"}, {"payload", {{"text", thought}}}};
              say(frame);
            });
      }

      // A remark about the reply goes out before the turn is closed, so it lands
      // under the text the client has already rendered.
      if (!result.notice.empty()) {
        boost::json::value frame = {{"type", "notice"}, {"payload", {{"text", result.notice}}}};
        say(frame);
      }

      boost::json::value final;
      if (result.has_error) {
        final = {{"type", "error"}, {"payload", {{"text", result.error_message}}}};
      } else if (result.model.empty()) {
        final = {{"type", "done"}};
      } else {
        // Who answered, which a gateway may have chosen rather than serve the
        // model that was configured.
        final = {{"type", "done"}, {"payload", {{"model", result.model}}}};
      }
      say(final);

      // The turn is over, and the store holds it now, so a client that arrives from
      // here on reads it from there: keeping the transcript would show the same
      // reply twice. Only the turn that owns the chat may say so, since a newer one
      // may already have taken it over.
      std::lock_guard<std::mutex> lock(active_ws->mtx);
      if (active_ws->cancel_token != token) return;
      active_ws->turn_in_flight = false;
      active_ws->transcript.clear();
    }).detach();
  }

  // The socket is gone. The turn it was watching is not: it is left to finish, and
  // the client that attaches next is handed what it has said so far.
  std::lock_guard<std::mutex> lock(active_ws->mtx);
  if (active_ws->client == ws) active_ws->client = nullptr;
}

}  // namespace pu::cli::detail
