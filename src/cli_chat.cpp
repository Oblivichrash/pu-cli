// SPDX-License-Identifier: GPL-3.0-only

#include "pu/cli_chat.hpp"

#include "pu/backend.hpp"
#include "pu/expert.hpp"
#include "pu/expert_config.hpp"
#include "pu/http/http_client.hpp"
#include "pu/renderer.hpp"
#include "pu/conversation_store.hpp"

#include "experts/chat/chat_expert.hpp"
#include "experts/bash/bash_expert.hpp"
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

namespace pu::cli {

namespace {

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string GenerateId() {
  auto now = std::chrono::high_resolution_clock::now();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now.time_since_epoch())
                   .count();
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
    if (!entry.description.empty()) {
      std::cout << " - " << entry.description;
    }
    if (entry.name == current) {
      std::cout << " [current]";
    }
    std::cout << "\n";
  }
}

void PrintConversationList(const std::vector<pu::Conversation>& convs) {
  if (convs.empty()) {
    std::cout << "No saved conversations.\n";
    return;
  }
  for (const auto& c : convs) {
    std::cout << "  " << c.id
              << " (" << c.messages.size() << " messages)"
              << " created: " << c.created_at
              << "\n";
  }
}

}  // namespace

int RunChatCommand(int argc, char* argv[]) {
  std::string initial_expert;
  bool show_reasoning = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu chat [--expert <name>] [--show-reasoning]\n";
      return 0;
    } else if (arg == "--expert") {
      if (i + 1 < argc) {
        initial_expert = argv[++i];
      } else {
        std::cerr << "Error: --expert requires an argument\n";
        return 1;
      }
    } else if (arg == "--show-reasoning") {
      show_reasoning = true;
    } else {
      std::cerr << "Error: unexpected argument '" << arg << "'\n";
      return 1;
    }
  }

  std::string config_path;
  try {
    config_path = pu::config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  pu::config::ExpertsConfig config;
  try {
    config = pu::config::LoadExpertsConfig(config_path);
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load config: " << e.what() << "\n";
    return 1;
  }

  if (config.experts.empty()) {
    std::cerr << "Error: no experts configured\n";
    return 1;
  }

  std::string current_name = initial_expert.empty() ? config.default_expert : initial_expert;
  const pu::config::ExpertEntry* current_entry = nullptr;
  for (const auto& entry : config.experts) {
    if (entry.name == current_name) {
      current_entry = &entry;
      break;
    }
  }
  if (!current_entry) {
    std::cerr << "Error: expert '" << current_name << "' not found\n";
    return 1;
  }

  auto chat_http = std::make_unique<pu::http::CurlHttpClient>();
  std::unique_ptr<pu::backend::Backend> chat_backend;
  try {
    chat_backend = pu::config::CreateBackend(current_entry->backend, std::move(chat_http));
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to create backend: " << e.what() << "\n";
    return 1;
  }

  pu::expert::ExpertManager manager;
  manager.RegisterExpert(
      std::make_unique<pu::experts::ChatExpert>("chat", std::move(chat_backend), current_entry->name));

  auto bash_http = std::make_unique<pu::http::CurlHttpClient>();
  std::unique_ptr<pu::backend::Backend> bash_backend;
  try {
    bash_backend = pu::config::CreateBackend(current_entry->backend, std::move(bash_http));
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to create bash backend: " << e.what() << "\n";
    return 1;
  }
  manager.RegisterExpert(
      std::make_unique<pu::experts::BashExpert>("bash", std::move(bash_backend),
                                                std::make_unique<pu::executor::CommandExecutor>(".")));

  if (!initial_expert.empty()) {
    manager.SetActiveExpert(initial_expert);
  }
  if (show_reasoning) {
    manager.SetShowReasoning(true);
  }

  auto store_dir = std::filesystem::path(
      std::getenv("HOME") ? std::getenv("HOME") : ".") / ".pu" / "conversations";
  pu::ConversationStore store(store_dir);

  std::vector<pu::ChatMessage> panel_messages;
  int message_id = 0;

  std::cout << "[INFO] Connected to expert: " << current_entry->name;
  if (!current_entry->description.empty()) {
    std::cout << " (" << current_entry->description << ")";
  }
  std::cout << "\nType /help for available commands.\n\n";

  std::string input;
  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, input)) {
      break;
    }
    if (input.empty()) {
      continue;
    }

    if (input[0] == '/') {
      if (input == "/help") {
        PrintHelp();
      } else if (input == "/exit" || input == "/quit") {
        break;
      } else if (input == "/clear") {
        manager.ClearSessions();
        panel_messages.clear();
        message_id = 0;
        std::cout << "[INFO] Conversation history and expert lock cleared.\n";
      } else if (input == "/experts") {
        PrintExperts(config, manager.GetActiveExpert());
      } else if (input.rfind("/expert ", 0) == 0) {
        std::string new_name = input.substr(8);
        size_t start = new_name.find_first_not_of(" \t");
        if (start != std::string::npos) {
          new_name = new_name.substr(start);
        } else {
          new_name.clear();
        }
        size_t end = new_name.find_last_not_of(" \t");
        if (end != std::string::npos) {
          new_name = new_name.substr(0, end + 1);
        } else {
          new_name.clear();
        }
        if (new_name.empty()) {
          std::cerr << "Error: expert name required.\n";
          continue;
        }

        const pu::config::ExpertEntry* new_entry = nullptr;
        for (const auto& entry : config.experts) {
          if (entry.name == new_name) {
            new_entry = &entry;
            break;
          }
        }
        if (!new_entry) {
          std::cerr << "Error: expert '" << new_name << "' not found.\n";
          continue;
        }

        manager.SetActiveExpert(new_entry->name);
        std::cout << "[INFO] Switched to expert: " << new_entry->name;
        if (!new_entry->description.empty()) {
          std::cout << " (" << new_entry->description << ")";
        }
        std::cout << "\n";
      } else if (input.rfind("/save", 0) == 0) {
        std::string save_name;
        if (input.size() > 5) {
          save_name = input.substr(6);
          size_t s = save_name.find_first_not_of(" \t");
          if (s != std::string::npos) {
            save_name = save_name.substr(s);
          } else {
            save_name.clear();
          }
          size_t e = save_name.find_last_not_of(" \t");
          if (e != std::string::npos) {
            save_name = save_name.substr(0, e + 1);
          } else {
            save_name.clear();
          }
        }
        if (save_name.empty()) {
          save_name = GenerateId();
        }

        pu::Conversation conv;
        conv.id = save_name;
        conv.created_at = panel_messages.empty()
                              ? CurrentTimestamp()
                              : panel_messages.front().timestamp;
        conv.updated_at = CurrentTimestamp();
        conv.messages = panel_messages;
        conv.expert_histories = manager.SnapshotExperts();

        try {
          store.Save(conv);
          std::cout << "[INFO] Conversation saved as '" << conv.id << "'\n";
        } catch (const std::exception& e) {
          std::cerr << "Error: failed to save conversation: " << e.what() << "\n";
        }
      } else if (input.rfind("/load ", 0) == 0) {
        std::string load_id = input.substr(6);
        size_t s = load_id.find_first_not_of(" \t");
        if (s != std::string::npos) {
          load_id = load_id.substr(s);
        } else {
          load_id.clear();
        }
        size_t e = load_id.find_last_not_of(" \t");
        if (e != std::string::npos) {
          load_id = load_id.substr(0, e + 1);
        } else {
          load_id.clear();
        }

        if (load_id.empty()) {
          std::cerr << "Error: conversation id required.\n";
          continue;
        }

        try {
          auto conv = store.Load(load_id);
          panel_messages = conv.messages;
          message_id = panel_messages.empty() ? 0 : panel_messages.back().id;
          manager.RestoreExperts(conv.expert_histories);
          manager.SetActiveExpert("");
          std::cout << "[INFO] Loaded conversation '" << load_id << "'\n";
        } catch (const std::exception& e) {
          std::cerr << "Error: failed to load conversation: " << e.what() << "\n";
        }
      } else if (input == "/list") {
        auto convs = store.List();
        PrintConversationList(convs);
      } else if (input.rfind("/export ", 0) == 0) {
        std::string export_id = input.substr(8);
        size_t s = export_id.find_first_not_of(" \t");
        if (s != std::string::npos) {
          export_id = export_id.substr(s);
        } else {
          export_id.clear();
        }
        size_t e = export_id.find_last_not_of(" \t");
        if (e != std::string::npos) {
          export_id = export_id.substr(0, e + 1);
        } else {
          export_id.clear();
        }

        if (export_id.empty()) {
          std::cerr << "Error: conversation id required.\n";
          continue;
        }

        try {
          std::string markdown = store.ExportMarkdown(export_id);
          std::string filename = "conversation_" + export_id + ".md";
          std::ofstream out(filename);
          if (!out) {
            std::cerr << "Error: cannot write to " << filename << "\n";
          } else {
            out << markdown;
            std::cout << "[INFO] Exported to " << filename << "\n";
          }
        } catch (const std::exception& e) {
          std::cerr << "Error: failed to export conversation: " << e.what() << "\n";
        }
      } else {
        std::cerr << "Unknown command: " << input << "\nType /help for available commands.\n";
      }
      continue;
    }

    std::string user_content = input;
    panel_messages.push_back({
      ++message_id,
      CurrentTimestamp(),
      "user",
      user_content
    });

    std::string reply_role;
    if (!input.empty() && input[0] == '@') {
      size_t space_pos = input.find(' ');
      if (space_pos != std::string::npos) {
        reply_role = input.substr(1, space_pos - 1);
      } else {
        reply_role = "chat";
      }
    } else {
      reply_role = manager.GetActiveExpert();
      if (reply_role.empty()) {
        reply_role = "chat";
      }
    }

    try {
      std::string response = manager.Dispatch(input);
      if (!response.empty()) {
        panel_messages.push_back({
          ++message_id,
          CurrentTimestamp(),
          reply_role,
          response
        });
      }
    } catch (const std::exception& e) {
      std::cerr << "\nError: " << e.what() << "\n\n";
    }
  }

  std::cout << "\nGoodbye!\n";
  return 0;
}

}  // namespace pu::cli
