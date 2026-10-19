// SPDX-License-Identifier: GPL-3.0-only

//
// The stdio server the client cases talk to, built as a program of its own.
//
// It used to run inside the test binary: `main()` checked a flag and became the server, so a
// machine with no interpreter could still exercise the subprocess path. That made the suite
// depend on its own executable in a way that kept biting — a binary that is being rewritten
// while a case starts it, or a case that inherits another case's idea of what `main` does.
// A separate 80-line program has none of that and needs no interpreter either.
//

#include "stdio_echo_server.h"

#include <boost/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

constexpr const char* kToolName = "clock";

boost::json::value Answer(const boost::json::value& request) {
  const boost::json::object& object = request.as_object();
  const int id = static_cast<int>(object.at("id").as_int64());
  const std::string method = boost::json::value_to<std::string>(object.at("method"));

  if (method == "initialize") {
    return boost::json::object{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result",
         boost::json::object{
             {"protocolVersion", "2024-11-05"},
             {"capabilities", boost::json::object{{"tools", boost::json::object{}}}},
             {"serverInfo", boost::json::object{{"name", "echo"}, {"version", "1"}}}}},
    };
  }
  if (method == "tools/list") {
    return boost::json::object{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result",
         boost::json::object{{"tools", boost::json::array{boost::json::object{
                                            {"name", kToolName},
                                            {"description", "Reads the current time"},
                                            {"inputSchema", boost::json::object{{"type", "object"}}},
                                        }}}}},
    };
  }
  if (method == "tools/call") {
    return boost::json::object{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result",
         boost::json::object{{"content", boost::json::array{boost::json::object{
                                              {"type", "text"},
                                              {"text", "the clock says noon"},
                                          }}}}},
    };
  }
  return boost::json::object{
      {"jsonrpc", "2.0"},
      {"id", id},
      {"error", boost::json::object{{"code", -32601}, {"message", "no such method"}}}};
}

}  // namespace

int main() {
  // One line in, one line out: the client reads a line at a time, so a reply that sits in a
  // buffer while the next request arrives is read as that request's answer.
  std::ios::sync_with_stdio(false);
  std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

  std::string line;
  while (std::getline(std::cin, line) && !line.empty()) {
    boost::json::value request;
    try {
      request = boost::json::parse(line);
    } catch (const std::exception&) {
      continue;
    }
    if (!request.is_object() || !request.as_object().contains("id")) continue;

    const std::string reply = boost::json::serialize(Answer(request));
    std::fwrite(reply.data(), 1, reply.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  return 0;
}
