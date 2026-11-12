// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/delegation.hpp"

// The fallback: a build that links the tools without the runtime can still say what a refusal
// means, so it needs the request type, and it must not link a turn it has no way to run. The
// runtime's own definition of this function replaces this one wherever it is linked, because a
// strong definition wins over a weak one.
namespace pu {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
DelegationResult RunDelegatedTurn(const Delegation& request) {
  (void)request;  // nothing here reads the question: no turn is run to ask it.
  DelegationResult result;
  result.error = "a delegated turn cannot run here: this build links the tools without the "
                 "runtime that carries one out";
  return result;
}

}  // namespace pu
