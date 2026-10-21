// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "tests/mocks/peer_harness.hpp"
#include "tests/mocks/serve_harness.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

namespace {

std::string FrameType(const boost::json::value& frame) {
  return boost::json::value_to<std::string>(frame.at("type"));
}

class Page {
 public:
  // `client` is the tab's name for itself: two Page objects with the same one are the same
  // page reconnecting, and an empty one is a client that names nothing.
  explicit Page(int port, const std::string& client = {}) : ws_(ioc_) {
    tcp::resolver resolver(ioc_);
    const auto endpoints = resolver.resolve(pu::tests::kServeHost, std::to_string(port));
    beast::get_lowest_layer(ws_).connect(endpoints);
    const std::string target = client.empty() ? "/ws" : "/ws?client=" + client;
    ws_.handshake(std::string(pu::tests::kServeHost) + ":" + std::to_string(port), target);
  }

  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;

  void Run(const std::string& text) {
    Send(boost::json::value{{"type", "run"}, {"payload", {{"text", text}}}});
  }

  void Cancel() { Send(boost::json::value{{"type", "cancel"}}); }

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

boost::json::value StoredConversation(pu::tests::ServeHarness& harness) {
  return boost::json::parse(harness.Client().Get("/api/history")).at("messages");
}

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

// A turn a peer asked runs with no page attached. When a page attaches afterwards it is handed
// the frames of that turn, so the reply is drawn without the page having been open while it ran
// and without a reload being the only way to see it.
TEST_CASE("A page that attaches after a peer's turn reads the reply", "[serve][ws]") {
  // The endpoint exists only where peers.json does, so the workspace names one before the
  // runtime reads it.
  pu::tests::ServeHarness harness("ollama", 0, std::nullopt, /*with_peers=*/true);
  auto client = harness.Client();

  // The endpoint queues the turn and answers 202; the backend's reply arrives a moment later.
  const auto accepted = client.PostRaw(
      "/api/peer/message", R"({"from":"A","message":"ping","hop":0})",
      pu::tests::kPeerToken);
  REQUIRE(accepted.status == 202);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto history = boost::json::parse(client.Get("/api/history")).at("messages");
    if (history.as_array().size() >= 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // The page that was not there while the turn ran attaches now and is handed the frames.
  Page page(harness.Port());
  const auto frames = page.ReadUntilEnd(5000);
  REQUIRE_FALSE(frames.empty());

  const bool saw_done = std::any_of(frames.begin(), frames.end(), [](const auto& frame) {
    return FrameType(frame) == "done";
  });
  CHECK(saw_done);
}

// A backend that fails under a running turn is the ordinary case. A failure the turn *reports*
// is survivable; one that unwinds out of the worker is not, because a thread that lets an
// exception through its entry function calls terminate, and that took the server down along
// with every page on it. A dead backend is the failure that reproduced it: the transport fails
// where no handler is left to turn it into a returned error.
TEST_CASE("A turn that throws ends the turn and not the server", "[serve][ws]") {
  pu::tests::ServeHarness harness{pu::tests::ServeHarness::UnreachableBackend{}};

  Page page(harness.Port());
  page.Run("hello");

  std::vector<boost::json::value> frames;
  for (;;) {
    const auto frame = page.Read(20000);
    if (!frame) break;
    frames.push_back(*frame);
    if (FrameType(*frame) == "error") break;
  }
  if (frames.empty()) {
    FAIL("no frames; backend_requests=" << harness.BackendRequests());
    return;
  }
  CHECK(FrameType(frames.back()) == "error");

  // The page survived the frame: a socket that took it as a malformed text frame would be gone.
  const auto after = harness.Client().GetRaw("/api/session");
  CHECK(after.status == 200);

  // And the worker is still there to take the turn after it.
  page.Run("hello again");
  const auto next = page.Read(20000);
  REQUIRE(next.has_value());
}

TEST_CASE("A run streams the answer and ends with done", "[serve][ws]") {
  pu::tests::ServeHarness harness;
  Page page(harness.Port());

  page.Run("hello");
  const auto frames = page.ReadUntilEnd(10000);

  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.back()) == "done");

  REQUIRE(ChunkText(frames) == "OK");
}

TEST_CASE("A run reports the tokens the backend counted", "[serve][ws]") {
  pu::tests::FakeBackend::ReportUsage(4242, 77);
  pu::tests::ServeHarness harness;

  Page page(harness.Port());
  page.Run("hello");
  const auto frames = page.ReadUntilEnd(10000);

  const auto usage = std::find_if(frames.begin(), frames.end(), [](const boost::json::value& frame) {
    return FrameType(frame) == "usage";
  });
  REQUIRE(usage != frames.end());
  REQUIRE(usage->at("payload").at("prompt_tokens").as_int64() == 4242);
  REQUIRE(usage->at("payload").at("completion_tokens").as_int64() == 77);

  // The counts describe the turn that ended, so they arrive before the turn is declared over.
  REQUIRE(FrameType(frames.back()) == "done");
  REQUIRE(FrameType(*(usage + 1)) == "done");

  pu::tests::FakeBackend::ReportUsage(1234, 56);
}

// The page draws two figures from a finished turn: what it cost, and what the conversation has
// spent. Both are known when the turn ends, so the frame carries both. A page told only the
// first has to ask again to learn the second, and a page that does not ask draws a total that
// stops short of the turn it just watched.
TEST_CASE("The frame a finished turn sends carries the total it is part of", "[serve][ws][usage]") {
  pu::tests::FakeBackend::ReportUsage(1000, 100);
  pu::tests::ServeHarness harness;
  Page page(harness.Port());

  page.Run("first");
  const auto first = page.ReadUntilEnd(10000);
  const auto first_usage =
      std::find_if(first.begin(), first.end(), [](const boost::json::value& frame) {
        return FrameType(frame) == "usage";
      });
  REQUIRE(first_usage != first.end());
  const boost::json::value& first_payload = first_usage->at("payload");
  REQUIRE(first_payload.as_object().contains("total"));
  CHECK(first_payload.at("total").at("turns").as_int64() == 1);
  CHECK(first_payload.at("total").at("prompt_tokens").as_int64() == 1000);

  // A second turn moves the total, and the frame says so rather than leaving the page to add
  // it up: the sums are the server's, and one of them is a price that does not always add.
  page.Run("second");
  const auto second = page.ReadUntilEnd(10000);
  const auto second_usage =
      std::find_if(second.begin(), second.end(), [](const boost::json::value& frame) {
        return FrameType(frame) == "usage";
      });
  REQUIRE(second_usage != second.end());
  const boost::json::value& second_payload = second_usage->at("payload");
  CHECK(second_payload.at("total").at("turns").as_int64() == 2);
  CHECK(second_payload.at("total").at("prompt_tokens").as_int64() == 2000);

  pu::tests::FakeBackend::ReportUsage(1234, 56);
}

// The frame and `/api/history` describe a turn to the same reader, so they must describe it
// with the same keys. A page that draws a live turn and a replayed one takes one path only if
// the two agree, and a field the frame forgets is a figure that vanishes on reload.
TEST_CASE("A turn is written the same way whether it just ended or is replayed",
          "[serve][ws][usage]") {
  pu::tests::FakeBackend::ReportUsage(3000, 120);
  pu::tests::ServeHarness harness;
  Page page(harness.Port());

  page.Run("hello");
  const auto frames = page.ReadUntilEnd(10000);
  const auto usage = std::find_if(frames.begin(), frames.end(), [](const boost::json::value& frame) {
    return FrameType(frame) == "usage";
  });
  REQUIRE(usage != frames.end());
  const boost::json::object& live = usage->at("payload").as_object();

  const boost::json::value history = boost::json::parse(harness.Client().Get("/api/history")).at("messages");
  REQUIRE(history.is_array());
  const boost::json::object* replayed = nullptr;
  for (const boost::json::value& item : history.as_array()) {
    if (item.as_object().contains("usage")) replayed = &item.at("usage").as_object();
  }
  REQUIRE(replayed != nullptr);

  // Every key the replay carries, the frame carries too, with the same value: both read the
  // ledger entry the turn was filed under, so a page takes one path whether a turn is live or
  // reloaded. The frame adds `total`, which describes a moment rather than a turn.
  for (const auto& field : *replayed) {
    const std::string key(field.key());
    INFO("key: " << key);
    REQUIRE(live.contains(key));
    CHECK(live.at(key) == field.value());
  }
  // The frame carries one field the replay does not, because it describes a moment.
  CHECK(live.contains("total"));
  CHECK_FALSE(replayed->contains("total"));

  pu::tests::FakeBackend::ReportUsage(1234, 56);
}

TEST_CASE("A second page is told the session is busy", "[serve][ws]") {
  pu::tests::ServeHarness harness;
  Page first(harness.Port());

  first.Run("mine");
  REQUIRE(FrameType(first.ReadUntilEnd(10000).back()) == "done");

  Page second(harness.Port());
  const auto refusal = second.Read(5000);
  REQUIRE(refusal.has_value());
  REQUIRE(FrameType(*refusal) == "busy");
  REQUIRE_FALSE(second.Read(5000).has_value());

  first.Run("still mine");
  const auto frames = first.ReadUntilEnd(10000);
  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.back()) == "done");
}

TEST_CASE("A second page is told the session is busy even when both name a tab",
          "[serve][ws]") {
  pu::tests::ServeHarness harness;
  Page first(harness.Port(), "tab-a");

  first.Run("mine");
  REQUIRE(FrameType(first.ReadUntilEnd(10000).back()) == "done");

  Page second(harness.Port(), "tab-b");
  const auto refusal = second.Read(5000);
  REQUIRE(refusal.has_value());
  REQUIRE(FrameType(*refusal) == "busy");

  first.Run("still mine");
  const auto frames = first.ReadUntilEnd(10000);
  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.back()) == "done");
}

// The watchdog on the page reopens a socket it believes is hung. The page is still the
// session's only client, so the new socket takes the session back instead of being refused.
TEST_CASE("The same page reconnecting takes the session back", "[serve][ws]") {
  pu::tests::ServeHarness harness;
  Page first(harness.Port(), "tab-a");

  first.Run("mine");
  REQUIRE(FrameType(first.ReadUntilEnd(10000).back()) == "done");

  // The page reopens its socket without closing the one it gave up on, which is what a
  // watchdog does when the close has not reached the server yet.
  Page reconnected(harness.Port(), "tab-a");
  reconnected.Run("mine again");
  const auto frames = reconnected.ReadUntilEnd(10000);
  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.front()) != "busy");
  REQUIRE(FrameType(frames.back()) == "done");
  REQUIRE(ChunkText(frames) == "OK");
}

// The page's watchdog gives up on a socket that went silent and opens another. The turn is
// still the same page's, so the replacement joins it rather than being refused, and the
// reply finishes on the new socket.
TEST_CASE("A page that reopens its socket mid-turn keeps reading the turn", "[serve][ws]") {
  pu::tests::ServeHarness harness("ollama", 600);
  Page first(harness.Port(), "tab-a");

  first.Run("hello");
  WaitForTheTurnToReachTheBackend(harness);  // the answer is being held back

  // The old socket is left open, the way a page that muted it would: the server closes it
  // when the replacement takes the session.
  Page reconnected(harness.Port(), "tab-a");
  const auto frames = reconnected.ReadUntilEnd(15000);

  REQUIRE_FALSE(frames.empty());
  REQUIRE(FrameType(frames.front()) != "busy");
  REQUIRE(FrameType(frames.back()) == "done");
  REQUIRE(ChunkText(frames) == "OK");
}

TEST_CASE("A page that goes away ends the turn and stores no reply", "[serve][ws]") {
  pu::tests::ServeHarness harness("ollama", 600);
  Page page(harness.Port());

  page.Run("hello");
  WaitForTheTurnToReachTheBackend(harness);  // the answer is being held back
  page.Abort();

  RequireOnlyTheQuestion(StoredConversation(harness), "hello");
}

TEST_CASE("A cancel ends the turn with done and keeps what already arrived", "[serve][ws]") {
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

  const auto history = StoredConversation(harness);
  REQUIRE(history.is_array());
  REQUIRE(history.as_array().size() == 2);
  REQUIRE(boost::json::value_to<std::string>(history.as_array()[0].at("role")) == "user");
  REQUIRE(boost::json::value_to<std::string>(history.as_array()[0].at("content")) == "hello");
  REQUIRE(boost::json::value_to<std::string>(history.as_array()[1].at("role")) == "assistant");
  REQUIRE(boost::json::value_to<std::string>(history.as_array()[1].at("content")) == "OK");
}
