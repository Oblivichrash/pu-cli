// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "tests/mocks/serve_harness.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace net = boost::asio;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

namespace {

std::string FrameType(const boost::json::value& frame) {
  return boost::json::value_to<std::string>(frame.at("type"));
}

// The page: the socket the front end opens, with the reads and writes a test needs. Every
// read is bounded, so a frame that never comes is a timeout rather than a hung test.
class Page {
 public:
  explicit Page(int port) : ws_(ioc_) {
    tcp::resolver resolver(ioc_);
    const auto endpoints = resolver.resolve(pu::tests::kServeHost, std::to_string(port));
    beast::get_lowest_layer(ws_).connect(endpoints);
    ws_.handshake(std::string(pu::tests::kServeHost) + ":" + std::to_string(port), "/ws");
  }

  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;

  void Run(const std::string& text) {
    Send(boost::json::value{{"type", "run"}, {"payload", {{"text", text}}}});
  }

  void Cancel() { Send(boost::json::value{{"type", "cancel"}}); }

  // One frame, or nothing when the wait ran out or the server closed the socket.
  std::optional<boost::json::value> Read(int timeout_ms) {
    beast::get_lowest_layer(ws_).expires_after(std::chrono::milliseconds(timeout_ms));
    beast::flat_buffer buffer;
    beast::error_code ec;
    ws_.read(buffer, ec);
    if (ec) return std::nullopt;
    try {
      return boost::json::parse(beast::buffers_to_string(buffer.data()));
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  // Everything up to and including the frame that ends the turn, or nothing at all when
  // the turn did not end within `timeout_ms`.
  std::vector<boost::json::value> ReadUntilEnd(int timeout_ms) {
    std::vector<boost::json::value> frames;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      const auto frame = Read(static_cast<int>(left));
      if (!frame) return {};
      frames.push_back(*frame);
      const std::string type = FrameType(*frame);
      if (type == "done" || type == "error") return frames;
    }
    return {};
  }

  // Closing the page, which is a socket that stops answering rather than a goodbye.
  void Abort() {
    beast::error_code ec;
    beast::get_lowest_layer(ws_).socket().close(ec);
  }

 private:
  void Send(const boost::json::value& frame) {
    ws_.write(net::buffer(boost::json::serialize(frame)));
  }

  net::io_context ioc_;
  websocket::stream<beast::tcp_stream> ws_;
};

std::string ChunkText(const std::vector<boost::json::value>& frames) {
  std::string text;
  for (const auto& frame : frames) {
    if (FrameType(frame) == "chunk") {
      text += boost::json::value_to<std::string>(frame.at("payload").at("text"));
    }
  }
  return text;
}

// What the conversation holds, read through the endpoint a reload uses. The request waits
// for a turn to let go of the session, so a turn in flight is over by the time this returns.
boost::json::value StoredConversation(pu::tests::ServeHarness& harness) {
  return boost::json::parse(harness.Client().Get("/api/history"));
}

// Waits until the turn has reached the backend, which is what makes it in flight: the
// conversation holds the question by then. A fixed sleep would be a race under load.
void WaitForTheTurnToReachTheBackend(pu::tests::ServeHarness& harness) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (harness.BackendRequests() > 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  FAIL("the turn never reached the backend");
}

void RequireOnlyTheQuestion(const boost::json::value& history, const std::string& question) {
  REQUIRE(history.is_array());
  REQUIRE(history.as_array().size() == 1);
  REQUIRE(boost::json::value_to<std::string>(history.as_array()[0].at("role")) == "user");
  REQUIRE(boost::json::value_to<std::string>(history.as_array()[0].at("content")) == question);
}

}  // namespace

TEST_CASE("A run streams the answer and ends with done", "[serve][ws]") {
  pu::tests::ServeHarness harness;
  Page page(harness.Port());

  page.Run("hello");
  const auto frames = page.ReadUntilEnd(10000);

  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.back()) == "done");

  // The reply arrives in chunks before the turn closes, and nothing arrives after it.
  REQUIRE(ChunkText(frames) == "OK");
}

TEST_CASE("A second page is told the session is busy", "[serve][ws]") {
  pu::tests::ServeHarness harness;
  Page first(harness.Port());

  // A completed turn is what shows the server has this page as its client.
  first.Run("mine");
  REQUIRE(FrameType(first.ReadUntilEnd(10000).back()) == "done");

  Page second(harness.Port());
  const auto refusal = second.Read(5000);
  REQUIRE(refusal.has_value());
  REQUIRE(FrameType(*refusal) == "busy");
  // The refusal closes the socket, rather than leaving a page that cannot be used.
  REQUIRE_FALSE(second.Read(5000).has_value());

  // The page that was there is undisturbed by the one that was turned away.
  first.Run("still mine");
  const auto frames = first.ReadUntilEnd(10000);
  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.back()) == "done");
}

TEST_CASE("A page that goes away ends the turn and stores no reply", "[serve][ws]") {
  pu::tests::ServeHarness harness("ollama", 600);
  Page page(harness.Port());

  page.Run("hello");
  WaitForTheTurnToReachTheBackend(harness);  // the answer is being held back
  page.Abort();

  RequireOnlyTheQuestion(StoredConversation(harness), "hello");
}

// What arrived before the stop may already have been shown; it is not an answer, so the
// conversation keeps the question alone. That is the rule, not the absence of frames.
TEST_CASE("A cancel ends the turn with done and stores no reply", "[serve][ws]") {
  pu::tests::ServeHarness harness("ollama", 600);
  Page page(harness.Port());

  page.Run("hello");
  WaitForTheTurnToReachTheBackend(harness);
  page.Cancel();

  const auto frames = page.ReadUntilEnd(10000);
  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.back()) == "done");
  for (const auto& frame : frames) {
    REQUIRE(FrameType(frame) != "error");
  }

  RequireOnlyTheQuestion(StoredConversation(harness), "hello");
}
