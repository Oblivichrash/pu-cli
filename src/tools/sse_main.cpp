// SPDX-License-Identifier: GPL-3.0-only

//
// `pu-sse`: read a server-sent event stream and print what arrives.
//
// It is the same client `pu sse` runs, reached without the rest of `pu`: no session, no
// configuration, no pager. A feed is not a chat turn, so reading one needs an endpoint and
// nothing that knows what a conversation is.
//

#include "pu/tools/sse.hpp"

#include <boost/program_options.hpp>

#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "pu/core/beast_http_client.hpp"
#include "pu/tools/entry_point.hpp"

namespace po = boost::program_options;

namespace {

using pu::tools::ExitCode;
using pu::tools::SseFormat;
using pu::tools::SseOptions;

po::options_description Options() {
  po::options_description options{"pu-sse options"};
  options.add_options()
      ("url", po::value<std::string>(), "the endpoint to read")
      ("header", po::value<std::vector<std::string>>()->multitoken(),
       "a request header, repeatable: --header 'Authorization: Bearer ...'")
      ("post", "ask with POST instead of GET")
      ("body", po::value<std::string>(), "request body to send; '-' reads it from stdin")
      ("format", po::value<std::string>()->default_value("text"),
       "text, json for one object per event, or raw for the bytes as sent")
      ("quiet", "print only the data, without the event and id line")
      ("help,h", "print this page");
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  // A bare `pu-sse` is not a request for the help page: it is a feed with no endpoint, which
  // the body refuses by name. So this one runs on an empty command line rather than printing
  // the options.
  SseOptions sse;

  return pu::tools::RunEntryPoint(
      argc, argv, Options(), pu::tools::BareInvocation::kRun,
      [&sse](const po::variables_map& vm) -> std::optional<int> {
        if (vm.count("url") == 0) {
          std::cerr << "a URL is needed: pu-sse <url> [options]\n";
          return static_cast<int>(ExitCode::kUsage);
        }
        sse.url = vm["url"].as<std::string>();

        if (vm.count("header") > 0) sse.headers = vm["header"].as<std::vector<std::string>>();

        if (vm.count("body") > 0) {
          const std::string given = vm["body"].as<std::string>();
          if (given == "-") {
            std::ostringstream body;
            body << std::cin.rdbuf();
            sse.body = body.str();
          } else {
            sse.body = given;
          }
        }

        sse.method =
            vm.count("post") > 0 ? pu::http::HttpMethod::kPost : pu::http::HttpMethod::kGet;

        const std::string format = vm["format"].as<std::string>();
        if (format == "json") {
          sse.format = SseFormat::kJson;
        } else if (format == "raw") {
          sse.format = SseFormat::kRaw;
        } else if (format != "text") {
          std::cerr << "--format is text, json or raw, not '" << format << "'\n";
          return static_cast<int>(ExitCode::kUsage);
        }
        sse.quiet = vm.count("quiet") > 0;
        return std::nullopt;
      },
      [&sse](const po::variables_map&) {
        pu::http::BeastHttpClient http;
        return static_cast<int>(pu::tools::RunSse(sse, http));
      });
}
