// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "pu/model/backend.hpp"
#include "pu/llm/model_factory.hpp"
#include "pu/tools_cli/exit_code.hpp"
#include "pu/tools_cli/models.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "tests/mocks/test_helpers.hpp"

using namespace pu;
using namespace pu::tests;

namespace {

bool HasHeader(const std::vector<std::string>& headers, const std::string& name) {
  return std::any_of(headers.begin(), headers.end(), [&name](const std::string& header) {
    return header.rfind(name + ":", 0) == 0;
  });
}

std::vector<std::string> IdsOf(const std::vector<model::ModelEntry>& entries) {
  std::vector<std::string> ids;
  ids.reserve(entries.size());
  for (const model::ModelEntry& entry : entries) ids.push_back(entry.id);
  return ids;
}

// `RunModels` writes its report to stdout, which is the contract the binary depends on. A test
// that called it directly would print a listing into the runner's output and bury the cases
// around it, so the stream is sent to a discarded buffer for the duration of each call. The
// report itself is not what these cases are about; the status is.
class QuietStdout {
 public:
  QuietStdout() : saved_(std::cout.rdbuf(sink_.rdbuf())) {}
  ~QuietStdout() { std::cout.rdbuf(saved_); }
  QuietStdout(const QuietStdout&) = delete;
  QuietStdout& operator=(const QuietStdout&) = delete;

 private:
  std::ostringstream sink_;
  std::streambuf* saved_;
};

// The listing a `--verify` run starts from, and the answer each model gives when asked for a
// token. `fail_at` is the 1-based model that throws instead of answering; 0 means none does.
tools_cli::ExitCode VerifyAgainst(const std::vector<std::string>& ids, int fail_at) {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [&ids](const std::string&, const std::vector<std::string>&) {
    std::string body = "{\"data\":[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (i > 0) body += ",";
      body += "{\"id\":\"" + ids[i] + "\"}";
    }
    body += "]}";
    return pu::http::Response{200, body};
  };
  int calls = 0;
  mock->simulate_response = [&calls, fail_at](const std::string&, const std::string&,
                                              const std::vector<std::string>&,
                                              pu::http::WriteCallback write_cb) {
    ++calls;
    if (fail_at != 0 && calls == fail_at) throw pu::HttpError("HTTP error 400: unavailable");
    const std::string frame = "data: {\"choices\":[{\"delta\":{\"content\":\"p\"}}]}\n\n";
    write_cb(const_cast<char*>(frame.data()), frame.size());
  };

  tools_cli::ModelsOptions options;
  options.verify = true;
  const QuietStdout quiet;
  return tools_cli::RunModels(options, BackendOfType(model::GatewayId::kOpenAI,
                                                     "https://x.example/v1"),
                              *http, http, std::nullopt);
}

}  // namespace

TEST_CASE("[list_models] ollama reads /api/tags and the models' names", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"models":[{"name":"gpt-oss:20b"},{"name":"q:2b"}]})"};
  };

  const auto models = llm::ListModels(
      BackendOfType(model::GatewayId::kOllama, "http://localhost:11434"), *mock);
  REQUIRE(IdsOf(models) == std::vector<std::string>{"gpt-oss:20b", "q:2b"});
  REQUIRE(mock->last_get_url == "http://localhost:11434/api/tags");
}

TEST_CASE("[list_models] openai reads /models and the data array's ids", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[{"id":"org/one"},{"id":"org/two"}]})"};
  };

  const auto models = llm::ListModels(
      BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"), *mock);
  REQUIRE(IdsOf(models) == std::vector<std::string>{"org/one", "org/two"});
  REQUIRE(mock->last_get_url == "https://x.example/v1/models");
}

TEST_CASE("[list_models] a declared window is read when no capability field named one",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[
      {"id":"with-window","context_window":8192},
      {"id":"without-window"}
    ]})"};
  };

  const auto models = llm::ListModels(
      BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"), *mock);
  REQUIRE(models.size() == 2);
  // A product that states a window declares the name in its shape; one that says nothing
  // leaves the answer at zero rather than inventing a default.
  REQUIRE(models[0].caps.max_input_tokens == 8192);
  REQUIRE(models[1].caps.max_input_tokens == 0);
}

TEST_CASE("[list_models] an api key rides the request as a bearer header", "[list_models]") {  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[]})"};
  };

  (void)llm::ListModels(
      BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1", "nvapi-secret"),
      *mock);
  REQUIRE(mock->last_headers.size() == 1);
  REQUIRE(mock->last_headers[0] == "Authorization: Bearer nvapi-secret");
}

TEST_CASE("[list_models] a listing that names no model is empty, not an error",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"models": null})"};
  };

  const auto models = llm::ListModels(
      BackendOfType(model::GatewayId::kOllama, "http://localhost:11434"), *mock);
  REQUIRE(models.empty());
}

TEST_CASE("[list_models] a body that does not parse is an Error, not a crash",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, "404 page not found"};
  };

  REQUIRE_THROWS_AS(llm::ListModels(
                        BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"),
                        *mock),
                    pu::GenericError);
}

TEST_CASE("[list_models] codebuddy offers the account's catalogue, then what only an agent lists",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  const std::string body =
      "{\"code\":0,\"data\":{"
      "\"agents\":[{\"name\":\"cli\",\"models\":[\"glm-5.2\",\"hy3\"]}],"
      "\"models\":[{\"id\":\"glm-5.2\"},{\"id\":\"hunyuan-image-alpha\"}]}}";
  mock->simulate_get = [body](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, body};
  };

  const std::string host = model::DefaultHostFor(model::GatewayId::kCodeBuddy);
  const auto models =
      llm::ListModels(BackendOfType(model::GatewayId::kCodeBuddy, host), *mock);

  REQUIRE(mock->last_get_url == "https://copilot.tencent.com/v3/config");
  // `data.models` first, in its own order, then the names only the agent lists. A reader
  // asking what they can pick was answered with the agent's subset alone, so a model the
  // account offers that no agent happened to name never reached the page.
  REQUIRE(IdsOf(models) ==
          std::vector<std::string>{"glm-5.2", "hunyuan-image-alpha", "hy3"});
  REQUIRE(HasHeader(mock->last_headers, "User-Agent"));
  REQUIRE(HasHeader(mock->last_headers, "X-User-Id"));
}

TEST_CASE("[list_models] a model in both codebuddy lists keeps the wider entry's capabilities",
          "[list_models]") {
  // The account's catalogue says what a model is; an agent's list only names it. When the two
  // name the same model the entry that knows something about itself has to win, or the picker
  // loses the window and the price of every model the agent also lists.
  auto [http, mock] = MakeMockHttpClient();
  const std::string body =
      "{\"data\":{"
      "\"agents\":[{\"models\":[\"glm-5.2\"]}],"
      "\"models\":[{\"id\":\"glm-5.2\",\"maxInputTokens\":128000,\"credits\":\"0.5\"}]}}";
  mock->simulate_get = [body](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, body};
  };

  const auto models = llm::ListModels(
      BackendOfType(model::GatewayId::kCodeBuddy, model::DefaultHostFor(model::GatewayId::kCodeBuddy)),
      *mock);
  REQUIRE(IdsOf(models) == std::vector<std::string>{"glm-5.2"});
  REQUIRE(models.at(0).caps.max_input_tokens == 128000);
  REQUIRE(models.at(0).caps.credits == "0.5");
}

TEST_CASE("[list_models] codebuddy with both lists null is an empty listing, not a refusal",
          "[list_models]") {
  // The shape a real account with no models configured answers: 200, `agents` and `models`
  // both null. This is the gateway saying it has nothing to offer, which is an answer, and it
  // must not print as a mismatch between this build and the product.
  auto [http, mock] = MakeMockHttpClient();
  const std::string body =
      "{\"code\":0,\"msg\":\"OK\",\"data\":{\"agent\":{\"agents\":null},\"models\":null,"
      "\"mcp\":{\"enableFilterCount\":0}}}";
  mock->simulate_get = [body](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, body};
  };

  const auto models = llm::ListModels(
      BackendOfType(model::GatewayId::kCodeBuddy, model::DefaultHostFor(model::GatewayId::kCodeBuddy)),
      *mock);
  REQUIRE(models.empty());
}

TEST_CASE("[list_models] a codebuddy key rides the config read too", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  const std::string body = "{\"data\":{\"agents\":[{\"models\":[]}]}}";
  mock->simulate_get = [body](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, body};
  };

  (void)llm::ListModels(
      BackendOfType(model::GatewayId::kCodeBuddy, "https://copilot.tencent.com/v2", "ck_x"),
      *mock);
  REQUIRE(HasHeader(mock->last_headers, "Authorization"));
}

TEST_CASE("[list_models] a container the answer does not carry is a refusal naming what it had",
          "[list_models]") {
  // A shape this build no longer matches must not read as an account offering nothing.
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"models_here":[{"name":"x"}]})"};
  };

  REQUIRE_THROWS_AS(
      llm::ListModels(BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"), *mock),
      pu::GenericError);

  try {
    (void)llm::ListModels(BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"),
                             *mock);
  } catch (const pu::GenericError& e) {
    const std::string message = e.what();
    REQUIRE(message.find("\"data\"") != std::string::npos);
    REQUIRE(message.find("models_here") != std::string::npos);
  }
}

TEST_CASE("[list_models] a container holding null is an empty listing, not a mismatch",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":null})"};
  };

  const auto models = llm::ListModels(
      BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"), *mock);
  REQUIRE(models.empty());
}

TEST_CASE("[list_models] --verify succeeds only when every model answered", "[list_models]") {
  // The whole point of `--verify` in a script is that its exit status distinguishes "all
  // listed models work" from "one of them does not". A run that reported a dead model and
  // still exited 0 would be worse than not checking at all.
  REQUIRE(VerifyAgainst({"m-one", "m-two"}, 0) == tools_cli::ExitCode::kOk);
  REQUIRE(VerifyAgainst({"m-one", "dead"}, 2) == tools_cli::ExitCode::kProvider);
}

TEST_CASE("[list_models] a listing that never arrives throws what the caller maps to 2",
          "[list_models]") {
  // The statuses separate "retry may work" from "a retry cannot help", so a provider that
  // could not be reached has to arrive at the caller as something it can tell apart from a
  // bad flag. `RunModels` lets the transport's `HttpError` out rather than swallowing it,
  // and `HttpError` is what both binaries catch to answer `kProvider`.
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&)
      -> pu::http::Response { throw pu::HttpError("HTTP connect timed out after 10s"); };

  tools_cli::ModelsOptions options;
  options.verify = true;
  const QuietStdout quiet;
  REQUIRE_THROWS_AS(llm::ListModels(BackendOfType(model::GatewayId::kOpenAI,
                                                  "https://x.example/v1"),
                                    *mock),
                    pu::HttpError);
  REQUIRE_THROWS_AS(tools_cli::RunModels(options,
                                         BackendOfType(model::GatewayId::kOpenAI,
                                                       "https://x.example/v1"),
                                         *http, http, std::nullopt),
                    pu::HttpError);
}

TEST_CASE("[list_models] --verify asks every model, so one refusal does not end the run",
          "[list_models]") {
  // `VerifyModels` reports a refusal as a result rather than a throw, and `RunModels` must
  // keep going past it: stopping at the first dead model would hide the ones after it, which
  // are the reason to run the check across a whole catalogue.
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[{"id":"dead"},{"id":"live"}]})"};
  };
  int posts = 0;
  mock->simulate_response = [&posts](const std::string&, const std::string&,
                                     const std::vector<std::string>&,
                                     pu::http::WriteCallback write_cb) {
    ++posts;
    if (posts == 1) throw pu::HttpError("HTTP error 400: Model unavailable");
    const std::string frame = "data: {\"choices\":[{\"delta\":{\"content\":\"p\"}}]}\n\n";
    write_cb(const_cast<char*>(frame.data()), frame.size());
  };

  tools_cli::ModelsOptions options;
  options.verify = true;
  const QuietStdout quiet;
  const tools_cli::ExitCode code = tools_cli::RunModels(
      options, BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"), *http, http,
      std::nullopt);

  REQUIRE(posts == 2);
  REQUIRE(code == tools_cli::ExitCode::kProvider);
}

TEST_CASE("[list_models] --entry with no table to copy from is a usage error",
          "[list_models]") {
  // The file being unreadable is an answer a script can act on, so it is a status rather than
  // an exception. `agents` is empty exactly when the caller could not read the entry.
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[{"id":"org/one"}]})"};
  };

  tools_cli::ModelsOptions options;
  options.shape = tools_cli::ModelShape::kEntry;
  options.entry_id = "org/one";
  const QuietStdout quiet;
  const tools_cli::ExitCode code = tools_cli::RunModels(
      options, BackendOfType(model::GatewayId::kOpenAI, "https://x.example/v1"), *http, http,
      std::nullopt);

  REQUIRE(code == tools_cli::ExitCode::kUsage);
}
