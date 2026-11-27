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
// turn can run. Why the chain is shaped this way, and what closes a cycle, is in
// docs/runtime.md#delegation.
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

  // The workspaces this question has already been asked in, oldest first, ending with the one
  // asking now. A target that appears here is refused as a cycle; empty means the top of a chain.
  std::vector<std::filesystem::path> visited;
};

struct DelegationResult {
  std::string answer;
  std::string error;                 // non-empty when the turn did not answer
  session::SessionUsageTotal usage;  // what that turn cost, in its own account
  // The caller stopped the turn. Kept apart from `error` so a deliberate stop does not read as a
  // fault; a tool has no vocabulary for it, so it also carries an `error` naming the stop.
  bool stopped = false;
};

// The workspace a path should compare as, on both sides of a chain. `weakly_canonical` resolves
// `..` and symlinks without requiring the path to exist. A path it cannot resolve is its own
// answer rather than an error.
std::filesystem::path AsOnePlace(const std::filesystem::path& where);

// Why `target` may not be asked, or an empty string when it may. The answer is about the chain
// alone — no runtime, no provider, no table — which is what lets it be checked before anything is
// built, since the cost of finding out by asking is a whole turn. The entries of `visited` are
// compared already-resolved.
std::string RefusedReEntry(const std::filesystem::path& target,
                           const std::vector<std::filesystem::path>& visited);

// Carries out one delegated turn. Declared here so the tool can call it, defined in the runtime.
DelegationResult RunDelegatedTurn(const Delegation& request);

}  // namespace pu
