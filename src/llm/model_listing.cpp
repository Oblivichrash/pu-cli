// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/model_listing.hpp"

#include "pu/core/base.hpp"
#include "pu/llm/codebuddy.hpp"
#include "pu/llm/codebuddy_catalog.hpp"

#include <boost/json.hpp>

namespace pu::llm {

namespace {

std::string AuthHeader(const std::string& api_key) {
  return api_key.empty() ? std::string{} : "Authorization: Bearer " + api_key;
}

// A Host, a listing body and its parse in one place: every lister does the same three
// steps and differs only in the URL read.
std::vector<std::string> ReadListing(const std::string& url, const std::vector<std::string>& headers,
                                     http::HttpClient& http, const char* container_key,
                                     const char* id_key) {
  const std::string body = http.Get(url, headers).body;
  if (body.empty()) return {};

  boost::json::value parsed;
  try {
    parsed = boost::json::parse(body);
  } catch (const boost::system::system_error&) {
    throw Error("Model listing from " + url + " does not parse as JSON");
  }
  if (!parsed.is_object()) return {};

  const auto& container = parsed.as_object();
  const auto it = container.find(container_key);
  if (it == container.end() || !it->value().is_array()) return {};

  std::vector<std::string> models;
  for (const auto& entry : it->value().as_array()) {
    if (!entry.is_object()) continue;
    const auto& fields = entry.as_object();
    const auto id = fields.find(id_key);
    if (id != fields.end() && id->value().is_string()) {
      models.push_back(boost::json::value_to<std::string>(id->value()));
    }
  }
  return models;
}

std::vector<std::string> ReadStrings(const boost::json::value& array) {
  if (!array.is_array()) return {};
  std::vector<std::string> values;
  for (const auto& entry : array.as_array()) {
    if (entry.is_string()) values.push_back(boost::json::value_to<std::string>(entry));
  }
  return values;
}

std::vector<std::string> ReadAgentModels(const boost::json::value& parsed) {
  if (!parsed.is_object()) return {};

  // The body is {"code":0,"data":{"agents":[...]}} - the agents sit one level in under
  // "data", not at the top, which is what the one-level listings below never have to face.
  const auto top = parsed.as_object();
  const auto payload = top.find("data");
  if (payload == top.end() || !payload->value().is_object()) return {};
  const auto& data = payload->value().as_object();

  const auto agents = data.find("agents");
  if (agents == data.end() || !agents->value().is_array()) return {};

  std::vector<std::string> models;
  for (const auto& agent : agents->value().as_array()) {
    if (!agent.is_object()) continue;
    const auto& fields = agent.as_object();
    const auto list = fields.find("models");
    if (list == fields.end()) continue;
    const std::vector<std::string> named = ReadStrings(list->value());
    models.insert(models.end(), named.begin(), named.end());
  }
  return models;
}

}  // namespace

std::vector<std::string> ListOllamaModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) {
  std::vector<std::string> headers;
  if (!api_key.empty()) headers.push_back(AuthHeader(api_key));
  return ReadListing(host + "/api/tags", headers, http, "models", "name");
}

std::vector<std::string> ListOpenAIModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) {
  std::vector<std::string> headers;
  if (!api_key.empty()) headers.push_back(AuthHeader(api_key));
  return ReadListing(host + "/models", headers, http, "data", "id");
}

std::vector<std::string> ListCodeBuddyModels(const std::string& host, const std::string& api_key,
                                             http::HttpClient& http) {
  const std::string url = CodeBuddyCatalogUrl(host);
  // The client's own header set: the User-Agent is what the gateway reads to answer a
  // catalogue at all rather than refusing the request.
  std::vector<std::string> headers = CodeBuddyHeaders(CodeBuddyConversationId());
  if (!api_key.empty()) headers.push_back(AuthHeader(api_key));

  const std::string body = http.Get(url, headers).body;
  if (body.empty()) return {};

  boost::json::value parsed;
  try {
    parsed = boost::json::parse(body);
  } catch (const boost::system::system_error&) {
    throw Error("Model listing from " + url + " does not parse as JSON");
  }

  // Every model the account is offered sits under data.models, images included, so this
  // does not go through ReadListing. Each agent's own models list is the subset a turn
  // can name, and those are the ids a caller choosing one wants.
  return ReadAgentModels(parsed);
}

std::vector<std::string> OllamaModelLister::ListModels(const std::string& host,
                                                       const std::string& api_key,
                                                       http::HttpClient& http) const {
  return ListOllamaModels(host, api_key, http);
}

std::vector<std::string> OpenAIModelLister::ListModels(const std::string& host,
                                                       const std::string& api_key,
                                                       http::HttpClient& http) const {
  return ListOpenAIModels(host, api_key, http);
}

std::vector<std::string> CodeBuddyModelLister::ListModels(const std::string& host,
                                                          const std::string& api_key,
                                                          http::HttpClient& http) const {
  return ListCodeBuddyModels(host, api_key, http);
}

}  // namespace pu::llm
