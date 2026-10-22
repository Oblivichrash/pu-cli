// SPDX-License-Identifier: GPL-3.0-only

//
// A host that accepts a connection and then says nothing used to hold its caller for as long as
// the process lived: `pu models` against such a gateway hung rather than failed, and so did a
// `pu ask` aimed at one.
//
// The deadline is proved against a real socket rather than a mock, because the whole difficulty
// is that asio's synchronous calls block where no timer can reach them — a mock has no `poll` in
// it and would pass whichever way the client was written.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pu/core/beast_http_client.hpp"
#include "pu/core/http_client.hpp"

namespace {

namespace net = boost::asio;
using tcp = net::ip::tcp;

// A listener that accepts and then neither reads nor writes, keeping each socket open for as
// long as it lives. What makes the client's read the thing that runs out of time.
class SilentServer {
 public:
  SilentServer() {
    acceptor_.open(tcp::v4());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(tcp::endpoint(net::ip::make_address("127.0.0.1"), 0));
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] {
      while (true) {
        tcp::socket socket(acceptor_.get_executor());
        boost::system::error_code ec;
        acceptor_.accept(socket, ec);
        if (ec) return;
        // Held: never read from, never written to, never closed.
        accepted_.push_back(std::move(socket));
      }
    });
  }

  ~SilentServer() {
    boost::system::error_code ec;
    acceptor_.close(ec);
    // A connect to the closed listener is refused at once, which is what wakes `accept`.
    tcp::socket wake(acceptor_.get_executor());
    wake.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port_), ec);
    if (thread_.joinable()) thread_.join();
  }

  SilentServer(const SilentServer&) = delete;
  SilentServer& operator=(const SilentServer&) = delete;

  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  net::io_context ioc_;
  tcp::acceptor acceptor_{ioc_};
  std::thread thread_;
  std::vector<tcp::socket> accepted_;
  std::uint16_t port_ = 0;
};

}  // namespace

// The budget is `kReadIdleBudget`, which is thirty seconds. Asserting the bound rather than the
// exact number keeps the case about "it ends" rather than about a figure tuned per release; the
// ceiling is generous enough that a slow machine is not a failure, and finite enough that a
// missing deadline is.
TEST_CASE("A read from a host that never answers comes back instead of hanging", "[http][deadline]") {
  SilentServer server;
  pu::http::BeastHttpClient client;

  const auto start = std::chrono::steady_clock::now();
  REQUIRE_THROWS_AS(client.Get(server.Url() + "/models", {}), pu::HttpError);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(elapsed < std::chrono::seconds(90));
  // And it is the deadline that ended it, not an immediate refusal: a client that gave up before
  // waiting at all would also be under the ceiling.
  CHECK(elapsed >= std::chrono::seconds(1));
}
