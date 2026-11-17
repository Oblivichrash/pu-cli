// SPDX-License-Identifier: GPL-3.0-only
#include "pu/runtime/answer.hpp"

#include <ostream>
#include <string>
#include <utility>

#include "pu/core/text.hpp"

namespace pu {

void PrintAnswer(const ExecutionResult& result, std::ostream& out) {
  if (result.has_error) return;
  if (result.was_streamed) {
    out << "\n";
  } else if (!result.content.empty()) {
    out << result.content << "\n";
  } else if (!result.was_command) {
    out << "\n";
  }
}

void PrintFailure(const ExecutionResult& result, std::ostream& err) {
  if (!result.has_error) return;
  err << text::ReadableError(result.error_message) << "\n";
}

}  // namespace pu
