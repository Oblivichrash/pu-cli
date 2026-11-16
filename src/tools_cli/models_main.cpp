// SPDX-License-Identifier: GPL-3.0-only

//
// `pu-models`: what a gateway offers, printed without the rest of `pu`.
//
// It is the same catalogue read `pu models` runs, reached without a session: it reads the agent
// table to find a gateway, and nothing else. A model list is not a conversation, so answering
// "what can I pick" needs a gateway and nothing that knows what a turn is.

#include "pu/tools_cli/models.hpp"

#include <boost/program_options.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "pu/config/agents.hpp"
#include "pu/config/backend_from_table.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/core/text.hpp"
#include "pu/model/backend.hpp"

namespace po = boost::program_options;

namespace {

using pu::tools_cli::AgentsSource;
using pu::tools_cli::ExitCode;
using pu::tools_cli::ModelsOptions;
using pu::tools_cli::ModelShape;

po::options_description Options() {
  po::options_description options{"pu-models options"};
  options.add_options()
      ("agent,a", po::value<std::string>(), "the agent whose backend to read (default: default_agent)")
      ("gateway,g", po::value<std::string>(), "read this gateway's own host and key instead")
      ("model,m", po::value<std::string>(), "the model to report with --gateway")
      ("detail", po::value<std::string>()->default_value("ids"),
       "ids for what you can pick, caps for the columns that decide a choice")
      ("entry,e", po::value<std::string>(),
       "print an agents.json entry for this model id, ready to paste")
      ("verify", "ask each listed model for one token and report what answered")
      ("config,c", po::value<std::string>(),
       "agents.json to read (default: ./.pu/agents.json, then ~/.pu/)")
      ("help,h", "print this page");
  return options;
}

// The backend a standalone catalogue read uses, resolved the same way `pu-probe` resolves
// one: without a session there is nothing to adopt, so the table is the whole of the answer.
pu::model::Backend BackendFor(const ModelsOptions& options, const std::string& config_path,
                              const std::string& agent_name) {
  return pu::config::BackendFromTable(options.gateway, options.model, agent_name, config_path);
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

  // A bare `pu-models` would otherwise read whatever agent happens to be the default, which is
  // a network call nobody asked for. Saying what the tool takes is the answer to "what does
  // this do"; the help page is one flag away.
  if (argc == 1) {
    std::cout << options << "\n";
    return static_cast<int>(ExitCode::kOk);
  }

  ModelsOptions models;
  if (vm.count("gateway") > 0) models.gateway = vm["gateway"].as<std::string>();
  if (vm.count("model") > 0) models.model = vm["model"].as<std::string>();
  models.verify = vm.count("verify") > 0;

  const std::string detail = vm["detail"].as<std::string>();
  if (detail == "caps") {
    models.shape = ModelShape::kCaps;
  } else if (detail == "ids") {
    models.shape = ModelShape::kIds;
  } else {
    std::cerr << "--detail is ids or caps, not '" << detail << "'\n";
    return static_cast<int>(ExitCode::kUsage);
  }
  if (vm.count("entry") > 0) {
    models.entry_id = vm["entry"].as<std::string>();
    models.shape = ModelShape::kEntry;
  }

  try {
    const std::string config_path = vm.count("config") > 0 ? vm["config"].as<std::string>()
                                                           : pu::config::FindConfigPath();
    // Which agent the entry is copied from: a named one, or the table's own default.
    const pu::config::AgentsConfig table = pu::config::LoadAgentsConfig(config_path);
    const std::string agent_name =
        vm.count("agent") > 0 ? vm["agent"].as<std::string>() : table.default_agent;

    const pu::model::Backend backend = BackendFor(models, config_path, agent_name);
    auto http = std::make_shared<pu::http::BeastHttpClient>();

    // An entry report reads the file a second time, through the same function `pu models`
    // uses, so the two print the same text for the same table.
    std::optional<AgentsSource> agents;
    if (models.shape == ModelShape::kEntry) {
      agents = pu::tools_cli::ReadAgentsSource(config_path, agent_name);
    }

    return static_cast<int>(pu::tools_cli::RunModels(models, backend, *http, http, agents));
  } catch (const std::exception& e) {
    std::cerr << pu::text::ReadableError(e.what()) << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }
}
