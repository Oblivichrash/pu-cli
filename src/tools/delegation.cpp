// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/delegation.hpp"

// The fallback: a build that links the tools without the runtime can still say what a refusal
// means, so it needs the request type, and it must not link a turn it has no way to run. The
// runtime's own definition of this function replaces this one wherever it is linked, because a
// strong definition wins over a weak one.
//
// MSVC has no equivalent of `__attribute__((weak))`, so on that toolchain two definitions of
// this symbol are two definitions, not a strong one overriding a weak one: it reports LNK2005.
// The weak definition is therefore only emitted where the attribute exists; a Windows build
// without the runtime does not link, which is the honest outcome, since this fallback exists to
// be replaced and MSVC cannot express that.
namespace pu {

std::filesystem::path AsOnePlace(const std::filesystem::path& where) {
  if (where.empty()) return where;
  std::error_code ec;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(where, ec);
  return ec ? where : resolved;
}

std::string RefusedReEntry(const std::filesystem::path& target,
                           const std::vector<std::filesystem::path>& visited) {
  if (visited.empty()) return {};
  // A question already asked there is the same question: the answer it would work towards is
  // one being worked towards further up the chain, so a second pass can only end at the
  // ceiling with nothing to show for it.
  const std::filesystem::path sees = AsOnePlace(target);
  for (const auto& before : visited) {
    if (AsOnePlace(before) == sees) {
      return "'" + target.string() +
             "' is already answering this question further up the chain, so asking it again "
             "would only wait for the answer that is already being worked on. Ask it something "
             "else, or ask the agent that sent you here";
    }
  }
  return {};
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
#if !defined(_MSC_VER)
DelegationResult RunDelegatedTurn(const Delegation& request) {
  (void)request;  // nothing here reads the question: no turn is run to ask it.
  DelegationResult result;
  result.error = "a delegated turn cannot run here: this build links the tools without the "
                 "runtime that carries one out";
  return result;
}
#endif  // !defined(_MSC_VER)

}  // namespace pu
