// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// One agent asking another, as data.
//
// These are plain structures and a single free function because two layers have to name them:
// the runtime that carries the turn out, and the tool that asks for it. They live beside the
// tool so that laying out a tool box never reaches up to the orchestration layer.
//
// `RunDelegatedTurn` is the seam that keeps the direction one-way. `runtime` defines it; a build
// that links only the tools links the fallback in `src/agent_tools/delegation.cpp`, which says no
// turn can run. A delegated turn registers no `ask_dir` of its own, which is what makes a chain
// exactly one hop; docs/runtime.md#delegation owns that rule.
//

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "pu/core/base.hpp"
#include "pu/session/usage.hpp"

namespace pu {

// One turn asked of an agent, without a conversation either side can remember. The caller states
// the whole question: what the turn reads on the way to its answer stays on the other side of
// that boundary.
struct Delegation {
  std::string agent;            // empty: the asking agent itself
  std::filesystem::path dir;    // empty: the asking runtime's workspace
  std::string prompt;

  // The asking turn's own stop, handed down, so it reaches the turn that is actually waiting.
  CancelToken cancel_token;

  // A ceiling on the whole delegated turn, which may run several rounds and call tools that take
  // minutes each. Zero means the asking turn's own platform default.
  std::chrono::milliseconds timeout{0};
};

struct DelegationResult {
  std::string answer;
  std::string error;                 // non-empty when the turn did not answer
  session::SessionUsageTotal usage;  // what that turn cost, in its own account
  // The caller stopped the turn. Kept apart from `error` so a deliberate stop does not read as a
  // fault; a tool has no vocabulary for it, so it also carries an `error` naming the stop.
  bool stopped = false;
};

// Carries out one delegated turn. Declared here so the tool can call it, defined in the runtime.
DelegationResult RunDelegatedTurn(const Delegation& request);

}  // namespace pu
