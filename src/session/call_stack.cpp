// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/call_stack.hpp"
#include "pu/session/workspace.hpp"
#include "pu/error.hpp"

namespace pu {

std::shared_ptr<CallStack> CallStack::Create(
  std::shared_ptr<Workspace> root_workspace) {
  auto stack = std::make_shared<CallStack>();
  stack->root_workspace_ = std::move(root_workspace);
  return stack;
}

void CallStack::Push(const Assignment& assignment, std::shared_ptr<Workspace> workspace) {
  Frame frame;
  frame.assignment = assignment;
  frame.workspace = workspace ? workspace : std::make_shared<Workspace>("ws-" + assignment.id);
  frames_.push_back(std::move(frame));
}

void CallStack::Push(const Assignment& assignment) {
  Push(assignment, nullptr);
}

std::optional<HandoffReceipt> CallStack::Pop() {
  if (frames_.empty()) {
    return std::nullopt;
  }

  Frame frame = std::move(frames_.back());
  frames_.pop_back();

  if (!frame.assignment.result.has_value()) {
    HandoffReceipt report;
    report.status = HandoffReceipt::kCompleted;
    report.summary = "[Delegation completed without explicit result]";
    frame.assignment.result = report;
  }

  return frame.assignment.result;
}

CallStack::Frame& CallStack::Current() {
  if (frames_.empty()) {
    throw RuntimeError("CallStack::Current() on empty stack");
  }
  return frames_.back();
}

const CallStack::Frame& CallStack::Current() const {
  if (frames_.empty()) {
    throw RuntimeError("CallStack::Current() on empty stack");
  }
  return frames_.back();
}

std::shared_ptr<Workspace> CallStack::CurrentWorkspace() const {
  if (frames_.empty()) {
    throw RuntimeError("CallStack::CurrentWorkspace() on empty stack");
  }
  return frames_.back().workspace;
}

size_t CallStack::Depth() const {
  return frames_.size();
}

bool CallStack::IsEmpty() const {
  return frames_.empty();
}

void CallStack::Clear() {
  frames_.clear();
}

nlohmann::json CallStack::Serialize() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& frame : frames_) {
    nlohmann::json f;
    f["assignment"] = frame.assignment.Serialize();
    if (frame.workspace) {
      f["workspace_id"] = frame.workspace->GetId();
    }
    arr.push_back(f);
  }
  return arr;
}

CallStack CallStack::Deserialize(const nlohmann::json& j) {
  CallStack cs;
  if (j.is_array()) {
    for (const auto& item : j) {
      Frame f;
      f.assignment = Assignment::Deserialize(item["assignment"]);
      cs.frames_.push_back(f);
    }
  }
  return cs;
}

} // namespace pu