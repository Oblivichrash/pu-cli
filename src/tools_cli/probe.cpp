// SPDX-License-Identifier: GPL-3.0-only
// The probe itself. It reads a stream and names what arrived; it opens no session, writes
// no transcript, and keeps no state between runs.

#include "pu/tools_cli/probe.hpp"

#include <filesystem>
#include <iostream>
#include <optional>

#include "pu/core/text.hpp"
#include "pu/llm/field_capture.hpp"

namespace pu::tools_cli {

ExitCode RunProbe(const ProbeOptions& options, const config::Backend& backend,
                  http::HttpClient& http) {
  // A catalogue read names no model; a turn does, and a gateway alone cannot say which.
  if (!options.listing && backend.model.empty()) {
    std::cerr << "a probe needs a model: pass --model, or --listing to read the catalogue\n";
    return ExitCode::kUsage;
  }

  if (options.listing) {
    // A listing is one answer rather than a stream, so there is nothing to keep as raw
    // bytes beyond the body the client already returned.
    const llm::ListingReport report = llm::ProbeListing(backend, http);
    std::cout << "\n" << llm::FormatListingReport(report) << "\n";
    return report.Succeeded() ? ExitCode::kOk : ExitCode::kProvider;
  }

  std::optional<std::string> raw_path;
  if (options.save) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_dir, ec);
    if (ec) {
      std::cerr << "cannot create " << options.out_dir << ": " << ec.message() << "\n";
      return ExitCode::kUsage;
    }
    raw_path = (std::filesystem::path(options.out_dir) /
                (std::string(config::GatewayName(backend.gateway)) + ".raw"))
                   .string();
    std::cout << "raw stream: " << *raw_path << "\n";
  }

  const std::string prompt = options.prompt.empty() ? "say hi" : options.prompt;
  const llm::FieldCapture report = llm::CaptureFields(backend, http, prompt, raw_path);
  std::cout << "\n" << llm::FormatFieldCapture(report) << "\n";
  return report.Succeeded() ? ExitCode::kOk : ExitCode::kProvider;
}

}  // namespace pu::tools_cli
