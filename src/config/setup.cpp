// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/setup.hpp"

#include "pu/core/base.hpp"
#include "pu/model/gateway.hpp"

#include <boost/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace pu::config {

namespace {

namespace json = pu::json;

std::string Trimmed(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

}  // namespace

void ValidateSetupChoice(const SetupChoice& choice) {
  const std::string agent = Trimmed(choice.agent_name);
  if (agent.empty()) {
    throw GenericError("an agent needs a name: it is what the session records as the one in use");
  }

  const std::string model = Trimmed(choice.model);
  if (model.empty()) {
    throw GenericError("a backend needs a model: run the listing and pick one, or type its id");
  }

  // A product that authenticates and a file with no key is a turn that fails on the first
  // request, far from this question. Asked here, while the reader is still at it -- but only
  // the variable being unset is that failure: a variable that holds a key is the answer the
  // question offered as the default, so a blank answer here is the intended one and the
  // written file names no key at all, leaving the run to read that variable.
  const std::string_view env_name = model::ApiKeyEnvFor(choice.gateway);
  if (Trimmed(choice.api_key).empty() && !env_name.empty()) {
    const char* from_env = std::getenv(std::string(env_name).c_str());
    if (from_env == nullptr || *from_env == '\0') {
      throw GenericError(std::string("this gateway reads a key from ") + std::string(env_name) +
                         "; enter one here or set that variable");
    }
  }
}

std::string RenderSetupConfig(const SetupChoice& choice) {
  // The single-agent form: no `agents` array and no `default_agent`, because a file that
  // describes one agent has nothing to choose between and nothing to default to. The name is
  // written because a session records it and there is no directory to supply it.
  json::object backend;
  backend["gateway"] = std::string(model::GatewayName(choice.gateway));
  if (!Trimmed(choice.host).empty()) backend["host"] = Trimmed(choice.host);
  backend["model"] = Trimmed(choice.model);
  // Written only when it is set: a local product's file says nothing about a key rather than
  // saying it has an empty one, which would read as a key that is wrong.
  if (!Trimmed(choice.api_key).empty()) backend["api_key"] = Trimmed(choice.api_key);

  json::object root;
  root["name"] = Trimmed(choice.agent_name);
  root["backend"] = std::move(backend);

  std::string text = boost::json::serialize(root);
  text += '\n';
  return text;
}

std::string WriteSetupConfig(const SetupChoice& choice, const std::string& workspace) {
  ValidateSetupChoice(choice);

  const std::filesystem::path dir = std::filesystem::path(workspace) / ".pu";
  const std::filesystem::path path = dir / "agents.json";

  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    throw GenericError("a configuration is already there: " + path.string());
  }
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    throw GenericError("could not create " + dir.string() + ": " + ec.message());
  }

  const std::string text = RenderSetupConfig(choice);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    throw GenericError("could not write " + path.string());
  }
  out << text;
  out.close();
  if (!out) {
    throw GenericError("could not write " + path.string());
  }
  return path.string();
}

}  // namespace pu::config
