// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>

#include "pu/llm/model_lister.hpp"

namespace pu::config {

enum class BackendType;

// The listing a backend type has. Every type whose host answers a catalogue is answered
// here, so a type that is not is a compile error rather than a null read.
std::unique_ptr<pu::ModelLister> FindModelListing(BackendType type);

}  // namespace pu::config
