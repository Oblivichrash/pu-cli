// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include <boost/json.hpp>

#include "pu/config/peer.hpp"
#include "pu/core/http_client.hpp"
#include "pu/tools/tool.hpp"

namespace pu::tools {

class PeerTool : public pu::Tool {
 public:
  PeerTool(const config::PeerConfig& peers, pu::http::HttpClient* client);

  std::string Name() const override;
  std::string Description() const override;
  boost::json::value ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;

 private:
  const config::PeerConfig& peers_;
  pu::http::HttpClient* client_;
};

}  // namespace pu::tools
