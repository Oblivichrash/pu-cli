// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// One agent asking another, as data.
//
// The request and the answer are plain structures because more than one side has to name them:
// the runtime that carries the turn out, and the tool that asks for it. They live here, beside
// the tool, rather than beside the runtime, so the tool states the question without including
// the thing that runs turns — and a module that lays out a tool box never has to reach up to
// the orchestration layer to name one.
//
// `RunDelegatedTurn` is the seam that keeps the direction one-way. The tool calls it; `runtime`
// is what defines it, and a program that links the runtime gets that definition. A program that
// links only the tools — the tests that check what a refusal says — links a fallback that says
// no turn can run, which is the truthful answer where no runtime was built.
//

#include <chrono>
#include <filesystem>
#include <string>

#include "pu/core/base.hpp"
#include "pu/session/usage.hpp"

namespace pu {

// One turn asked of an agent, without a conversation either side can remember. The caller
// states the whole question, because the agent it asks has no idea what this conversation has
// said: that is the boundary, and what the turn reads on the way to its answer stays on the
// other side of it.
//
// A delegated turn gets no `ask_dir` of its own, so a chain of them stops at one hop, and what
// it cost comes back, so the turn that asked can account for what it spent.
struct Delegation {
  std::string agent;            // empty: the table's own default_agent
  std::filesystem::path dir;    // empty: the asking runtime's workspace
  std::string prompt;

  // The asking turn's own stop, handed down rather than copied: the point is that a stop reaches
  // the turn that is actually waiting, which is this one. Checked by the same code the asking
  // turn checks its own token with, so a stop lands between rounds and between tool calls, and
  // a provider read is abandoned at the next fragment.
  CancelToken cancel_token;

  // A ceiling on the whole delegated turn, since it may run several rounds and call tools that
  // take minutes each. Zero means the ceiling the asking turn's own platform applies, which for
  // a stream is an idle deadline per read rather than a cap on the reply.
  std::chrono::milliseconds timeout{0};
};

struct DelegationResult {
  std::string answer;
  std::string error;                 // non-empty when the turn did not answer
  session::SessionUsageTotal usage;  // what that turn cost, in its own account
  // The turn was asked to stop by the caller rather than finished or failed. A tool has no
  // vocabulary for "you were stopped", so this reads as an error naming the stop; it is kept
  // apart from one so a caller that stopped deliberately does not read it as a fault.
  bool stopped = false;
};

// Carries out one delegated turn. Declared here so the tool can call it, defined in the runtime
// that knows how to build and drive one. A build without that runtime links the fallback in
// `src/tools/delegation.cpp`, which answers that no turn ran rather than pretending one did.
DelegationResult RunDelegatedTurn(const Delegation& request);

}  // namespace pu
