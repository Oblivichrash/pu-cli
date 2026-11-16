// SPDX-License-Identifier: GPL-3.0-only

//
// `pu-probe`: send one prompt to a gateway and print what the stream carried.
//
// It is the same capture `pu probe` runs, reached without the rest of `pu`: it reads the
// agent table to find a gateway, and nothing else. No session, no transcript, no pager —
// a probe answers "what does this host send", which is a question about a wire format and
// not about a conversation.
//

#include "pu/tools_cli/probe.hpp"

#include <boost/program_options.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "pu/config/agents.hpp"
#include "pu/config/backend_from_table.hpp"
#include "pu/model/backend.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/core/text.hpp"

namespace po = boost::program_options;

namespace {

using pu::tools_cli::ExitCode;
using pu::tools_cli::ProbeOptions;

po::options_description Options() {
  po::options_description options{"pu-probe options"};
  options.add_options()
      ("gateway", po::value<std::string>(),
       "the gateway to read, as named in the agent table")
      ("model", po::value<std::string>(), "the model to ask; required unless --listing")
      ("prompt,p", po::value<std::string>()->default_value("say hi"),
       "the single prompt to send")
      ("agent", po::value<std::string>(), "the agent whose keys and gateway to use")
      ("out-dir", po::value<std::string>()->default_value("probe"),
       "where to write the raw stream")
      ("save", "keep the raw stream, not just the report")
      ("listing", "read the model catalogue instead of sending a prompt")
      ("help,h", "print this page");
  return options;
}

// The backend a standalone probe reads, from the table alone: there is no session to adopt, so
// the agent the table names is the one `pu probe` would have called its current one. The rule
// lives in `backend_from_table`, where `pu-models` reads it too.
pu::model::Backend BackendFor(const ProbeOptions& options) {
  return pu::config::BackendFromTable(options.gateway, options.model, options.agent,
                                      pu::config::FindConfigPath());
}

}  // namespace

int main(int argc, char** argv) {
  po::options_description options = Options();
  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv).options(options).run(), vm);
    po::notify(vm);
  } catch (const std::exception& e) {
    std::cerr << pu::text::ReadableError(e.what()) << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }

  if (vm.count("help") > 0) {
    std::cout << options << "\n";
    return static_cast<int>(ExitCode::kOk);
  }

  // A bare `pu-probe` would otherwise send a prompt to whatever agent happens to be the
  // default, which is a network call nobody asked for. Saying what the tool takes is the
  // answer to "what does this do"; the help page is one flag away.
  if (argc == 1) {
    std::cout << options << "\n";
    return static_cast<int>(ExitCode::kOk);
  }

  ProbeOptions probe;
  if (vm.count("gateway") > 0) probe.gateway = vm["gateway"].as<std::string>();
  if (vm.count("model") > 0) probe.model = vm["model"].as<std::string>();
  if (vm.count("agent") > 0) probe.agent = vm["agent"].as<std::string>();
  probe.prompt = vm["prompt"].as<std::string>();
  probe.out_dir = vm["out-dir"].as<std::string>();
  probe.save = vm.count("save") > 0;
  probe.listing = vm.count("listing") > 0;

  try {
    const pu::model::Backend backend = BackendFor(probe);
    pu::http::BeastHttpClient http;
    return static_cast<int>(pu::tools_cli::RunProbe(probe, backend, http));
  } catch (const std::exception& e) {
    std::cerr << pu::text::ReadableError(e.what()) << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }
}
