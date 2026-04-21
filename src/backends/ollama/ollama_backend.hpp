// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Ollama backend implementation.

#ifndef PU_BACKENDS_OLLAMA_BACKEND_HPP
#define PU_BACKENDS_OLLAMA_BACKEND_HPP

#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include <memory>
#include <optional>
#include <string>

namespace pu::backends::ollama {

class OllamaBackend : public pu::backend::Backend {
 public:
  explicit OllamaBackend(Config config,
                         std::unique_ptr<pu::http::HttpClient> http);
  ~OllamaBackend() override = default;

  void Chat(const std::vector<Message>& history, ChatCallback cb) override;

 private:
  struct SseToken {
    std::string content;
    bool done = false;
  };
  static std::optional<SseToken> ParseSseLine(std::string_view line);
  std::string BuildRequest(const std::vector<Message>& history) const;

  Config config_;
  std::unique_ptr<pu::http::HttpClient> http_;
};

}  // namespace pu::backends::ollama

#endif  // PU_BACKENDS_OLLAMA_BACKEND_HPP
