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

#include <optional>
#include <string>

#include "pu/config/agents.hpp"
#include "pu/config/backend_from_table.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/model/backend.hpp"
#include "pu/tools_cli/entry_point.hpp"

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
  return pu::tools_cli::RunEntryPoint(
      argc, argv, Options(), pu::tools_cli::BareInvocation::kPrintHelp,
      // Nothing to check before running: `pu-probe` reads the table for a gateway and
      // accepts a prompt it will not send unless asked to.
      [](const po::variables_map&) -> std::optional<int> { return std::nullopt; },
      [](const po::variables_map& vm) {
        ProbeOptions probe;
        if (vm.count("gateway") > 0) probe.gateway = vm["gateway"].as<std::string>();
        if (vm.count("model") > 0) probe.model = vm["model"].as<std::string>();
        if (vm.count("agent") > 0) probe.agent = vm["agent"].as<std::string>();
        probe.prompt = vm["prompt"].as<std::string>();
        probe.out_dir = vm["out-dir"].as<std::string>();
        probe.save = vm.count("save") > 0;
        probe.listing = vm.count("listing") > 0;

        const pu::model::Backend backend = BackendFor(probe);
        pu::http::BeastHttpClient http;
        return static_cast<int>(pu::tools_cli::RunProbe(probe, backend, http));
      });
}
