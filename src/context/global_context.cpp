// SPDX-License-Identifier: GPL-3.0-only
#include "pu/context.hpp"

namespace pu {

std::shared_ptr<GlobalContext> GlobalContext::Create() {
  return std::shared_ptr<GlobalContext>(new GlobalContext());
}

std::optional<json> GlobalContext::Read(const std::string& path) const {
  (void)path;
  return std::nullopt;
}

void GlobalContext::Write(const std::string& path, const json& value) {
  (void)path;
  (void)value;
}

void GlobalContext::LoadFromDisk(const std::filesystem::path& data_dir) {
  (void)data_dir;
}

void GlobalContext::SaveToDisk(const std::filesystem::path& data_dir) const {
  (void)data_dir;
}

}  // namespace pu
