// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/model_listing.hpp"

#include "pu/core/base.hpp"
#include "pu/llm/codebuddy_catalog.hpp"
#include "pu/llm/model_lister.hpp"

#include <boost/json.hpp>

#include <sstream>

namespace pu::llm {

namespace {

std::string AuthHeader(const std::string& api_key) {
  return api_key.empty() ? std::string{} : "Authorization: Bearer " + api_key;
}

const boost::json::value* Field(const boost::json::object& object, const std::string& name) {
  if (name.empty()) return nullptr;
  const auto it = object.find(name);
  return it == object.end() ? nullptr : &it->value();
}

bool BoolField(const boost::json::object& object, const std::string& name) {
  const boost::json::value* value = Field(object, name);
  return value != nullptr && value->is_bool() && value->as_bool();
}

std::string StringField(const boost::json::object& object, const std::string& name) {
  const boost::json::value* value = Field(object, name);
  return value != nullptr && value->is_string() ? boost::json::value_to<std::string>(*value)
                                                : std::string{};
}

int IntField(const boost::json::object& object, const std::string& name) {
  const boost::json::value* value = Field(object, name);
  return value != nullptr && value->is_int64() ? static_cast<int>(value->as_int64()) : 0;
}

Capabilities ReadCapabilities(const boost::json::object& entry, const CatalogShape& shape) {
  Capabilities caps;
  caps.supports_tools = BoolField(entry, shape.supports_tools);
  caps.supports_images = BoolField(entry, shape.supports_images);
  caps.supports_reasoning = BoolField(entry, shape.supports_reasoning);
  caps.only_reasoning = BoolField(entry, shape.only_reasoning);
  caps.max_input_tokens = IntField(entry, shape.max_input_tokens);

  if (const boost::json::value* parent = Field(entry, shape.effort_parent);
      parent != nullptr && parent->is_object()) {
    caps.default_effort = StringField(parent->as_object(), shape.effort_field);
  }
  return caps;
}

std::vector<std::string> ReadStrings(const boost::json::value& array) {
  if (!array.is_array()) return {};
  std::vector<std::string> values;
  for (const auto& entry : array.as_array()) {
    if (entry.is_string()) values.push_back(boost::json::value_to<std::string>(entry));
  }
  return values;
}

boost::json::value ParseListing(const std::string& url, http::HttpClient& http,
                                const std::vector<std::string>& headers) {
  const std::string body = http.Get(url, headers).body;
  if (body.empty()) return nullptr;
  try {
    return boost::json::parse(body);
  } catch (const boost::system::system_error&) {
    throw Error("Model listing from " + url + " does not parse as JSON");
  }
}

const boost::json::array* ArrayAt(const boost::json::value& parsed, const std::string& key) {
  if (!parsed.is_object()) return nullptr;
  const auto it = parsed.as_object().find(key);
  if (it == parsed.as_object().end() || !it->value().is_array()) return nullptr;
  return &it->value().as_array();
}

std::vector<ModelEntry> ReadFlat(const boost::json::value& parsed, const CatalogShape& shape) {
  const boost::json::array* container = ArrayAt(parsed, shape.container);
  if (container == nullptr) return {};

  std::vector<ModelEntry> models;
  for (const auto& entry : *container) {
    if (!entry.is_object()) continue;
    const auto& fields = entry.as_object();
    const std::string id = StringField(fields, shape.id_key);
    if (id.empty()) continue;
    models.push_back(ModelEntry{id, ReadCapabilities(fields, shape)});
  }
  return models;
}

std::vector<ModelEntry> ReadCodeBuddy(const boost::json::value& parsed, const CatalogShape& shape) {
  if (!parsed.is_object()) return {};
  const auto top = parsed.as_object();

  const boost::json::value* payload = nullptr;
  if (const auto it = top.find("data"); it != top.end() && it->value().is_object()) {
    payload = &it->value();
  }
  if (payload == nullptr) return {};

  std::vector<std::string> ids;
  if (const boost::json::array* agents = ArrayAt(*payload, shape.container); agents != nullptr) {
    for (const auto& agent : *agents) {
      if (!agent.is_object()) continue;
      if (const boost::json::value* list = Field(agent.as_object(), shape.nested);
          list != nullptr) {
        const std::vector<std::string> named = ReadStrings(*list);
        ids.insert(ids.end(), named.begin(), named.end());
      }
    }
  }

  std::vector<ModelEntry> models;
  for (const std::string& id : ids) {
    ModelEntry entry;
    entry.id = id;
    if (const boost::json::array* catalogue = ArrayAt(*payload, shape.id_key);
        catalogue != nullptr) {
      for (const auto& item : *catalogue) {
        if (!item.is_object()) continue;
        const auto& fields = item.as_object();
        if (StringField(fields, "id") == id) {
          entry.caps = ReadCapabilities(fields, shape);
          break;
        }
      }
    }
    models.push_back(std::move(entry));
  }
  return models;
}

boost::json::value FetchListing(std::string_view url, const std::string& api_key,
                                const std::vector<std::string>& headers,
                                http::HttpClient& http) {
  std::vector<std::string> all = headers;
  if (!api_key.empty()) all.push_back(AuthHeader(api_key));
  return ParseListing(std::string(url), http, all);
}

}  // namespace

std::vector<ModelEntry> OllamaModelLister::ListModels(const std::string& host,
                                                      const std::string& api_key,
                                                      http::HttpClient& http) const {
  static const CatalogShape kShape = {
      .path = "/api/tags",
      .container = "models",
      .id_key = "name",
  };
  return ReadFlat(FetchListing(host + kShape.path, api_key, {}, http), kShape);
}

std::vector<ModelEntry> OpenAIModelLister::ListModels(const std::string& host,
                                                      const std::string& api_key,
                                                      http::HttpClient& http) const {
  static const CatalogShape kShape = {
      .path = "/models",
      .container = "data",
      .id_key = "id",
  };
  return ReadFlat(FetchListing(host + kShape.path, api_key, {}, http), kShape);
}

CodeBuddyModelLister::CodeBuddyModelLister(std::vector<std::string> headers)
    : headers_(std::move(headers)) {}

std::vector<ModelEntry> CodeBuddyModelLister::ListModels(const std::string& host,
                                                         const std::string& api_key,
                                                         http::HttpClient& http) const {
  static const CatalogShape kShape = {
      .container = "agents",
      .id_key = "models",
      .nested = "models",
      .supports_tools = "supportsToolCall",
      .supports_images = "supportsImages",
      .supports_reasoning = "supportsReasoning",
      .only_reasoning = "onlyReasoning",
      .effort_parent = "reasoning",
      .effort_field = "effort",
      .max_input_tokens = "maxInputTokens",
  };
  return ReadCodeBuddy(FetchListing(CodeBuddyCatalogUrl(host), api_key, headers_, http), kShape);
}

std::string FormatModelList(const std::vector<ModelEntry>& models,
                            std::string_view gateway,
                            std::string_view host,
                            std::string_view active_model) {
  if (models.empty())
    return "No models reported by " + std::string(gateway) + " at " + std::string(host);

  std::ostringstream oss;
  oss << models.size() << " model(s) reported by " << gateway << " at " << host << ":\n";
  for (const ModelEntry& model : models) {
    oss << "  " << model.id;
    if (model.id == active_model) oss << " (active)";
    oss << "\n";
  }
  return oss.str();
}

}  // namespace pu::llm
