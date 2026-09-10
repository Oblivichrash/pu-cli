// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <memory>

namespace pu {

// Shared cancellation token polled at every layer to abort an in-flight
// request. It is transport-agnostic (LLM providers and the HTTP client both
// observe it), so it lives in the base layer rather than next to the HTTP
// adapter.
using CancelToken = std::shared_ptr<std::atomic<bool>>;

}  // namespace pu
