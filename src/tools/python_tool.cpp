// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/python_tool.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <thread>
#include <future>
#include <regex>

namespace pu::tools {

PythonTool::PythonTool(const std::string& file_path) : file_path_(file_path) {
  std::ifstream file(file_path);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open Python tool file: " + file_path);
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
  Parse(content);
}

void PythonTool::Parse(const std::string& content) {
  std::istringstream iss(content);
  std::string line;
  bool in_code_block = false;
  std::ostringstream code_builder;

  while (std::getline(iss, line)) {
    if (!in_code_block) {
      if (line.find("# tool:") == 0) {
        name_ = line.substr(7);
        name_.erase(0, name_.find_first_not_of(" \t"));
        name_.erase(name_.find_last_not_of(" \t") + 1);
      } else if (line.find("# description:") == 0) {
        description_ = line.substr(14);
        description_.erase(0, description_.find_first_not_of(" \t"));
        description_.erase(description_.find_last_not_of(" \t") + 1);
      } else if (line.find("# parameters:") == 0) {
        parameters_schema_ = line.substr(13);
        parameters_schema_.erase(0, parameters_schema_.find_first_not_of(" \t"));
        parameters_schema_.erase(parameters_schema_.find_last_not_of(" \t") + 1);
      } else if (line.find("# timeout:") == 0) {
        std::string val = line.substr(10);
        val.erase(0, val.find_first_not_of(" \t"));
        timeout_seconds_ = std::stoi(val);
      } else if (line.find("# output_limit:") == 0) {
        std::string val = line.substr(15);
        val.erase(0, val.find_first_not_of(" \t"));
        output_limit_ = std::stoul(val);
      } else if (line.find("def run(") != std::string::npos) {
        in_code_block = true;
        code_builder << line << "\n";
      }
    } else {
      code_builder << line << "\n";
    }
  }

  python_code_ = code_builder.str();
  if (name_.empty() || description_.empty() || parameters_schema_.empty() || python_code_.empty()) {
    throw std::runtime_error("Invalid Python tool definition in " + file_path_);
  }
}

std::string PythonTool::Name() const { return name_; }
std::string PythonTool::Description() const { return description_; }
std::string PythonTool::ParametersSchema() const { return parameters_schema_; }

std::string PythonTool::Execute(const nlohmann::json& args, agent::ToolContext& ctx) {
  (void)ctx;
  std::string args_json = args.dump();
  return ExecutePython(args_json);
}

std::string PythonTool::ExecutePython(const std::string& args_json) const {
  auto tmp_dir = std::filesystem::temp_directory_path();
  auto script_path = tmp_dir / ("pu_tool_" + name_ + ".py");
  std::ofstream script(script_path);
  if (!script.is_open()) {
    return "Error: cannot create temporary script";
  }

  script << python_code_ << "\n\n";
  script << "import json, sys\n";
  script << "if __name__ == '__main__':\n";
  script << "    args = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}\n";
  script << "    result = run(**args)\n";
  script << "    print(result)\n";
  script.close();

  std::string cmd = "python3 " + script_path.string() + " '" + args_json + "' 2>&1";

  auto future = std::async(std::launch::async, [&]() -> std::string {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "Error: failed to execute python";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
      result += buffer.data();
    }
    int status = pclose(pipe);
    if (status != 0 && result.empty()) {
      return "Python script failed (exit " + std::to_string(status) + ")";
    }
    return result;
  });

  if (future.wait_for(std::chrono::seconds(timeout_seconds_)) != std::future_status::ready) {
    std::filesystem::remove(script_path);
    return "Error: Python script timed out after " + std::to_string(timeout_seconds_) + " seconds";
  }

  std::string result = future.get();
  std::filesystem::remove(script_path);

  if (result.size() > output_limit_) {
    result = result.substr(0, output_limit_) + "\n... (truncated)";
  }
  if (!result.empty() && result.back() == '\n') result.pop_back();
  return result;
}

}  // namespace pu::tools
