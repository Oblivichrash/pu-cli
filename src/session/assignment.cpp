// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/assignment.hpp"
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace pu {

std::string Assignment::GenerateId() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%d%H%M%S");

  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(1000, 9999);
  ss << "-" << dis(gen);

  return "asgn-" + ss.str();
}

bool Assignment::IsTimeout() const {
  if (deadline.time_since_epoch().count() == 0) return false;
  return std::chrono::steady_clock::now() > deadline;
}

nlohmann::json Assignment::Serialize() const {
  nlohmann::json j;
  j["id"] = id;
  j["goal"] = goal;
  j["agent_name"] = agent_name;
  nlohmann::json arts = nlohmann::json::array();
  for (const auto& a : seeded_artifacts) {
    arts.push_back(a.Serialize());
  }
  j["seeded_artifacts"] = arts;
  j["constraints"] = constraints;
  j["depth"] = depth;
  if (result.has_value()) {
    j["result"] = result->Serialize();
  }
  return j;
}

Assignment Assignment::Deserialize(const nlohmann::json& j) {
  Assignment a;
  a.id = j.value("id", "");
  a.goal = j.value("goal", "");
  a.agent_name = j.value("agent_name", "");
  if (j.contains("seeded_artifacts") && j["seeded_artifacts"].is_array()) {
    for (const auto& item : j["seeded_artifacts"]) {
      a.seeded_artifacts.push_back(Artifact::Deserialize(item));
    }
  }
  if (j.contains("constraints") && j["constraints"].is_array()) {
    a.constraints = j["constraints"].get<std::vector<std::string>>();
  }
  a.depth = j.value("depth", 0);
  if (j.contains("result") && !j["result"].is_null()) {
    a.result = HandoffReceipt::Deserialize(j["result"]);
  }
  return a;
}

} // namespace pu
