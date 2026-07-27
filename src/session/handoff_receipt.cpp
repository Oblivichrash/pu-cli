// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/handoff_receipt.hpp"

namespace pu {

nlohmann::json HandoffReceipt::Serialize() const {
  nlohmann::json j;
  j["status"] = static_cast<int>(status);
  j["summary"] = summary;
  nlohmann::json disc = nlohmann::json::array();
  for (const auto& a : key_discoveries) {
    disc.push_back(a.Serialize());
  }
  j["key_discoveries"] = disc;
  j["unresolved"] = unresolved;
  j["duration_ms"] = duration.count();
  return j;
}

HandoffReceipt HandoffReceipt::Deserialize(const nlohmann::json& j) {
  HandoffReceipt r;
  r.status = static_cast<Status>(j.value("status", 0));
  r.summary = j.value("summary", "");
  if (j.contains("key_discoveries") && j["key_discoveries"].is_array()) {
    for (const auto& item : j["key_discoveries"]) {
      r.key_discoveries.push_back(Artifact::Deserialize(item));
    }
  }
  if (j.contains("unresolved") && j["unresolved"].is_array()) {
    r.unresolved = j["unresolved"].get<std::vector<std::string>>();
  }
  r.duration = std::chrono::milliseconds(j.value("duration_ms", 0));
  return r;
}

} // namespace pu
