// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pu {

struct StackFrame {
  std::string agent_name;
  std::string invocation_id;
  std::vector<std::string> write_paths;
  std::optional<std::string> output_path;
};

class CallStack {
 public:
  void Push(const StackFrame& frame);
  StackFrame Pop();
  StackFrame& Top();
  bool IsEmpty() const;
  size_t Size() const;
  const std::vector<StackFrame>& GetFrames() const;

 private:
  std::vector<StackFrame> frames_;
};

}  // namespace pu
