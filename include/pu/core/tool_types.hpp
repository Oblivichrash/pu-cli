// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// What a model may call, and what it called, as data.
//
// A tool is named and described in the same shape whether the provider sent the call, an MCP
// server published the tool, or this program's own toolbox answers it — so the shape lives
// here, below all three, rather than in the module that happened to need it first. Nothing
// below `llm` then has to include a provider vocabulary to say what a tool is.
//

#include <string>

#include <boost/json.hpp>

namespace pu {

struct ToolDefinition {
  std::string name;
  std::string description;
  boost::json::value parameters;

  const boost::json::value& Parameters() const {
    static const boost::json::value kEmpty = boost::json::object{};
    return parameters.is_object() ? parameters : kEmpty;
  }
};

struct ToolCall {
  std::string id;
  std::string name;
  boost::json::value arguments;
};

}  // namespace pu
