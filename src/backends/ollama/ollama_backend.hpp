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
#include <string>

namespace pu::backends::ollama {

class OllamaBackend : public pu::backend::Backend {
 public:
  struct Config : public pu::backend::Backend::Config {
    std::string host = "http://localhost:11434";
  };

  explicit OllamaBackend(Config config,
                         std::unique_ptr<pu::http::HttpClient> http);
  ~OllamaBackend() override = default;

  void Chat(const std::vector<pu::backend::Message>& history,
            pu::backend::ChatCallback cb) override;

 private:
  std::string BuildRequest(const std::vector<pu::backend::Message>& history) const;

  std::string host_;
  std::unique_ptr<pu::http::HttpClient> http_;
};

}  // namespace pu::backends::ollama

#endif  // PU_BACKENDS_OLLAMA_BACKEND_HPP
