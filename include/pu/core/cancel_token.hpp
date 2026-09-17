// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <memory>

namespace pu {

// Shared cancel flag observed by every layer (LLM providers and the HTTP
// client), so it lives in the base layer rather than next to the HTTP adapter.
using CancelToken = std::shared_ptr<std::atomic<bool>>;

}  // namespace pu
