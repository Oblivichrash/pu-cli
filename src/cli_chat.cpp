// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli_chat.hpp"
#include "pu/backend.hpp"
#include "pu/expert.hpp"
#include "pu/expert_config.hpp"
#include "pu/http/http_client.hpp"
#include "pu/renderer.hpp"
#include "pu/conversation_store.hpp"
#include "pu/cli_app_setup.hpp"
#include "http/curl_http_client.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <system_error>

namespace pu::cli {

namespace {

inline std::string Trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return {};
  auto end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string GenerateId() {
  auto now = std::chrono::high_resolution_clock::now();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  std::ostringstream ss;
  ss << std::hex << nanos;
  return "conv-" + ss.str();
}

void PrintHelp() {
  std::cout << "Available commands:\n"
            << "  /help           Show this help\n"
            << "  /exit, /quit    Exit interactive mode\n"
            << "  /clear          Clear conversation history and expert lock\n"
            << "  /expert <name>  Switch to different expert\n"
            << "  /experts        List available experts\n"
            << "  /proactive <expert> on|off [threshold]  Control proactive suggestions\n"
            << "  /save [name]    Save current conversation\n"
            << "  /load <id>      Load a saved conversation\n"
            << "  /list           List saved conversations\n"
            << "  /export <id>    Export conversation to Markdown\n"
            << "  --show-reasoning (startup flag) Show model reasoning\n";
}

void PrintExperts(const pu::config::ExpertsConfig& config, const std::string& current) {
  std::cout << "Available experts:\n";
  for (const auto& entry : config.experts) {
    std::cout << "  " << entry.name;
    if (!entry.description.empty()) std::cout << " - " << entry.description;
    if (entry.name == current) std::cout << " [current]";
    std::cout << "\n";
  }
}

void PrintConversationList(const std::vector<pu::Conversation>& convs) {
  if (convs.empty()) { std::cout << "No saved conversations.\n"; return; }
  for (const auto& c : convs)
    std::cout << "  " << c.id << " (" << c.messages.size() << " messages)"
              << " created: " << c.created_at << "\n";
}

}  // anonymous namespace

int RunChatCommand(int argc, char* argv[]) {
  std::string initial_expert;
  bool show_reasoning = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu chat [--expert <name>] [--show-reasoning]\n";
      return 0;
    } else if (arg == "--expert") {
      if (i + 1 < argc) initial_expert = argv[++i];
      else { std::cerr << "Error: --expert requires an argument\n"; return 1; }
    } else if (arg == "--show-reasoning") { show_reasoning = true; }
    else { std::cerr << "Error: unexpected argument '" << arg << "'\n"; return 1; }
  }

  auto ctx = SetupAppContext(initial_expert, show_reasoning);
  const auto& config = ctx.config;
  auto& manager = ctx.manager;
  std::string current_name = manager.GetActiveExpert();

  const char* home = std::getenv("HOME");
  auto store_dir = std::filesystem::path(home ? home : ".") / ".pu" / "conversations";
  pu::ConversationStore store(store_dir);
  std::vector<pu::ChatMessage> panel_messages;
  int message_id = 0;

  struct ConfirmationState {
    bool auto_approve_safe = false;
    bool deny_all = false;
  };
  auto confirm_state = std::make_shared<ConfirmationState>();

  manager.SetConfirmationCallback([confirm_state](const pu::expert::ConfirmationRequest& req) {
    if (confirm_state->deny_all) return pu::expert::ConfirmationChoice::kDenyAll;
    if (confirm_state->auto_approve_safe &&
        req.highest_risk == pu::executor::RiskLevel::kSafe)
      return pu::expert::ConfirmationChoice::kApproveOnce;
    std::cout << "[CONFIRM] " << req.description << " [y/N/a(all safe)/s(deny all)] ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer == "a") {
      confirm_state->auto_approve_safe = true;
      return req.highest_risk == pu::executor::RiskLevel::kSafe
                 ? pu::expert::ConfirmationChoice::kApproveOnce
                 : pu::expert::ConfirmationChoice::kDeny;
    }
    if (answer == "s") { confirm_state->deny_all = true; return pu::expert::ConfirmationChoice::kDenyAll; }
    return (answer == "y" || answer == "Y") ? pu::expert::ConfirmationChoice::kApproveOnce
                                            : pu::expert::ConfirmationChoice::kDeny;
  });

  std::cout << "[INFO] Connected to expert: " << current_name;
  for (const auto& e : config.experts) {
    if (e.name == current_name && !e.description.empty())
      std::cout << " (" << e.description << ")";
  }
  std::cout << "\nType /help for available commands.\n\n";

  std::string input;
  while (std::cout << "> " << std::flush, std::getline(std::cin, input)) {
    if (input.empty()) continue;
    if (input[0] == '/') {
      if (input == "/help") PrintHelp();
      else if (input == "/exit" || input == "/quit") break;
      else if (input == "/clear") {
        manager.ClearSessions();
        panel_messages.clear();
        message_id = 0;
        confirm_state->auto_approve_safe = false;
        confirm_state->deny_all = false;
        std::cout << "[INFO] Conversation history and expert lock cleared.\n";
      } else if (input == "/experts") PrintExperts(config, manager.GetActiveExpert());
      else if (input.rfind("/expert ", 0) == 0) {
        auto new_name = Trim(input.substr(8));
        if (new_name.empty()) { std::cerr << "Error: expert name required.\n"; continue; }
        const pu::config::ExpertEntry* new_entry = nullptr;
        for (const auto& entry : config.experts) {
          if (entry.name == new_name) { new_entry = &entry; break; }
        }
        if (!new_entry) { std::cerr << "Error: expert '" << new_name << "' not found.\n"; continue; }
        manager.SetActiveExpert(new_entry->name);
        current_name = new_name;
        std::cout << "[INFO] Switched to expert: " << new_entry->name;
        if (!new_entry->description.empty()) std::cout << " (" << new_entry->description << ")";
        std::cout << "\n";
      } else if (input.rfind("/proactive ", 0) == 0) {
        std::istringstream iss(input.substr(11));
        std::string expert, state;
        double threshold = 0.6;
        iss >> expert >> state;
        if (expert.empty() || state.empty()) {
          std::cerr << "Usage: /proactive <expert> on|off [threshold]\n"; continue;
        }
        bool enable = (state == "on");
        if (!enable && state != "off") { std::cerr << "State must be 'on' or 'off'.\n"; continue; }
        if (enable && !iss.eof()) { std::string t; iss >> t; if (!t.empty()) threshold = std::stod(t); }
        bool found = false;
        for (const auto& e : config.experts) if (e.name == expert) { found = true; break; }
        if (!found) { std::cerr << "Error: expert '" << expert << "' not found.\n"; continue; }
        if (enable) { manager.SetProactiveEnabled(true); manager.SetProactiveThreshold(threshold); }
        else manager.SetProactiveEnabled(false);
        std::cout << "[INFO] Proactive suggestions " << (enable ? "enabled" : "disabled") << ".\n";
      } else if (input.rfind("/save", 0) == 0) {
        auto save_name = (input.size() > 5) ? Trim(input.substr(6)) : std::string{};
        if (save_name.empty()) save_name = GenerateId();
        pu::Conversation conv;
        conv.id = save_name;
        conv.created_at = panel_messages.empty() ? CurrentTimestamp() : panel_messages.front().timestamp;
        conv.updated_at = CurrentTimestamp();
        conv.messages = panel_messages;
        conv.expert_histories = manager.SnapshotExperts();
        std::error_code ec;
        store.Save(conv, ec);
        if (ec) std::cerr << "Error: failed to save conversation: " << ec.message() << "\n";
        else std::cout << "[INFO] Conversation saved as '" << conv.id << "'\n";
      } else if (input.rfind("/load ", 0) == 0) {
        auto load_id = Trim(input.substr(6));
        if (load_id.empty()) { std::cerr << "Error: conversation id required.\n"; continue; }
        std::error_code ec;
        auto conv = store.Load(load_id, ec);
        if (ec) std::cerr << "Error: failed to load conversation: " << ec.message() << "\n";
        else {
          panel_messages = conv.messages;
          message_id = panel_messages.empty() ? 0 : panel_messages.back().id;
          manager.RestoreExperts(conv.expert_histories);
          manager.SetActiveExpert("");
          confirm_state->auto_approve_safe = false;
          confirm_state->deny_all = false;
          std::cout << "[INFO] Loaded conversation '" << load_id << "'\n";
        }
      } else if (input == "/list") PrintConversationList(store.List());
      else if (input.rfind("/export ", 0) == 0) {
        auto export_id = Trim(input.substr(8));
        if (export_id.empty()) { std::cerr << "Error: conversation id required.\n"; continue; }
        std::error_code ec;
        auto md = store.ExportMarkdown(export_id, ec);
        if (ec) std::cerr << "Error: failed to export conversation: " << ec.message() << "\n";
        else {
          std::string filename = "conversation_" + export_id + ".md";
          std::ofstream out(filename);
          if (!out) std::cerr << "Error: cannot write to " << filename << "\n";
          else { out << md; std::cout << "[INFO] Exported to " << filename << "\n"; }
        }
      } else std::cerr << "Unknown command: " << input << "\nType /help for available commands.\n";
      continue;
    }

    panel_messages.push_back({++message_id, CurrentTimestamp(), "user", input, ""});
    manager.NotifyPanelMessage(panel_messages.back());

    std::string reply_role = manager.GetActiveExpert();
    if (!input.empty() && input[0] == '@') {
      auto space_pos = input.find(' ');
      reply_role = (space_pos != std::string::npos) ? input.substr(1, space_pos - 1) : "chat";
    }
    if (reply_role.empty()) reply_role = "chat";

    try {
      auto response = manager.Dispatch(input);
      if (!response.empty()) {
        panel_messages.push_back({++message_id, CurrentTimestamp(), reply_role, response, ""});
        manager.NotifyPanelMessage(panel_messages.back());

        std::vector<pu::ChatMessage> recent;
        auto start_idx = panel_messages.size() > 20 ? panel_messages.size() - 20 : 0;
        for (size_t i = start_idx; i < panel_messages.size(); ++i)
          recent.push_back(panel_messages[i]);
        manager.SetRecentMessages(recent);
        for (auto& [expert, text] : manager.CollectProactiveReplies()) {
          panel_messages.push_back({++message_id, CurrentTimestamp(), expert, text, ""});
          std::cout << "\n[" << expert << "] " << text << std::endl;
          manager.NotifyPanelMessage(panel_messages.back());
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "\nError: " << e.what() << "\n\n";
    }
  }
  std::cout << "\nGoodbye!\n";
  return 0;
}

}  // namespace pu::cli
