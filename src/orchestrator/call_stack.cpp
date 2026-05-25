// SPDX-License-Identifier: GPL-3.0-only
#include "pu/stack.hpp"

#include <stdexcept>

namespace pu {

void CallStack::Push(const StackFrame& frame) {
  frames_.push_back(frame);
}

StackFrame CallStack::Pop() {
  if (frames_.empty()) {
    throw std::runtime_error("CallStack::Pop() on empty stack");
  }
  StackFrame frame = std::move(frames_.back());
  frames_.pop_back();
  return frame;
}

StackFrame& CallStack::Top() {
  if (frames_.empty()) {
    throw std::runtime_error("CallStack::Top() on empty stack");
  }
  return frames_.back();
}

bool CallStack::IsEmpty() const {
  return frames_.empty();
}

size_t CallStack::Size() const {
  return frames_.size();
}

}  // namespace pu
