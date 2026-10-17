// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/model_listing.hpp"

#include "pu/core/base.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/llm/model_listing.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <sstream>

namespace pu::llm {

namespace {

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
  caps.credits = StringField(entry, shape.credits);

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
    throw GenericError("Model listing from " + url + " does not parse as JSON");
  }
}

// So a shape that no longer matches never prints as "the account offers no model".
std::string TopLevelKeys(const boost::json::value& parsed) {
  const boost::json::object* obj = parsed.if_object();
  if (obj == nullptr) return "(not a JSON object)";

  std::string keys;
  for (const auto& member : *obj) {
    if (!keys.empty()) keys += ", ";
    keys += std::string(member.key());
  }
  return keys.empty() ? "(an empty object)" : keys;
}

const boost::json::array* ArrayAt(const boost::json::value& parsed, const std::string& key) {
  if (!parsed.is_object()) return nullptr;
  const auto it = parsed.as_object().find(key);
  if (it == parsed.as_object().end() || !it->value().is_array()) return nullptr;
  return &it->value().as_array();
}

// An absent key is a gateway this build cannot read and is refused; a key holding null is
// the gateway saying it has nothing, which is an answer.
std::optional<boost::json::array> ContainerOr(const boost::json::value& parsed,
                                             const std::string& key, const char* url) {
  const boost::json::object* obj = parsed.if_object();
  if (obj != nullptr) {
    if (const auto it = obj->find(key); it != obj->end()) {
      if (it->value().is_array()) return it->value().as_array();
      if (it->value().is_null()) return boost::json::array{};
    }
  }
  throw GenericError("Model listing from " + std::string(url) + " carries no \"" + key +
                     "\" array; its top-level keys were: " + TopLevelKeys(parsed));
}

std::vector<ModelEntry> ReadFlat(const boost::json::value& parsed, const CatalogShape& shape,
                                 const std::string& url) {
  const std::optional<boost::json::array> container =
      ContainerOr(parsed, shape.container, url.c_str());

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

std::vector<ModelEntry> ReadCodeBuddy(const boost::json::value& parsed, const CatalogShape& shape,
                                     const std::string& url) {
  const boost::json::object* top = parsed.if_object();
  const boost::json::value* payload = nullptr;
  if (top != nullptr) {
    if (const auto it = top->find("data"); it != top->end() && it->value().is_object()) {
      payload = &it->value();
    }
  }
  if (payload == nullptr) {
    throw GenericError("Model listing from " + url + " carries no \"data\" object; its top-level "
                       "keys were: " + TopLevelKeys(parsed));
  }

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
  if (!api_key.empty()) all.push_back(http::BearerHeader(api_key));
  return ParseListing(std::string(url), http, all);
}

}  // namespace

namespace {

const CatalogShape& OllamaShape() {
  static const CatalogShape shape = {
      .path = "/api/tags",
      .container = "models",
      .id_key = "name",
  };
  return shape;
}

const CatalogShape& OpenAIShape() {
  static const CatalogShape shape = {
      .path = "/models",
      .container = "data",
      .id_key = "id",
  };
  return shape;
}

const CatalogShape& CodeBuddyShape() {
  static const CatalogShape shape = {
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
      .credits = "credits",
  };
  return shape;
}

}  // namespace

std::optional<ListingTarget> ListingTargetFor(std::string_view protocol, std::string_view gateway,
                                              const std::string& host) {
  if (protocol == "ollama") return ListingTarget{host + OllamaShape().path, OllamaShape()};
  if (protocol != "openai") return std::nullopt;
  // One OpenAI-format product keeps its catalogue off the chat base's origin and describes a
  // model with more than an id; the rest read `/models` and name what they can.
  if (gateway == "codebuddy") return ListingTarget{CodeBuddyCatalogUrl(host), CodeBuddyShape()};
  return ListingTarget{host + OpenAIShape().path, OpenAIShape()};
}

std::vector<ModelEntry> OllamaModelLister::ListModels(const std::string& host,
                                                      const std::string& api_key,
                                                      http::HttpClient& http) const {
  const std::string url = host + OllamaShape().path;
  return ReadFlat(FetchListing(url, api_key, {}, http), OllamaShape(), url);
}

std::vector<ModelEntry> OpenAIModelLister::ListModels(const std::string& host,
                                                      const std::string& api_key,
                                                      http::HttpClient& http) const {
  const std::string url = host + OpenAIShape().path;
  return ReadFlat(FetchListing(url, api_key, {}, http), OpenAIShape(), url);
}

CodeBuddyModelLister::CodeBuddyModelLister(std::vector<std::string> headers)
    : headers_(std::move(headers)) {}

std::vector<ModelEntry> CodeBuddyModelLister::ListModels(const std::string& host,
                                                         const std::string& api_key,
                                                         http::HttpClient& http) const {
  const std::string url = CodeBuddyCatalogUrl(host);
  return ReadCodeBuddy(FetchListing(url, api_key, headers_, http), CodeBuddyShape(), url);
}

std::string FormatModelList(const std::vector<ModelEntry>& models,
                            std::string_view gateway,
                            std::string_view host,
                            std::string_view active_model) {
  // A listing is the subset its agent may run, which the backend's own model can sit outside;
  // the running model is named rather than dropped from the one row that matters.
  const bool active_listed =
      std::any_of(models.begin(), models.end(),
                  [&](const ModelEntry& model) { return model.id == active_model; });

  if (models.empty())
    return "No models reported by " + std::string(gateway) + " at " + std::string(host);

  std::ostringstream oss;
  oss << models.size() << " model(s) reported by " << gateway << " at " << host << ":\n";
  for (const ModelEntry& model : models) {
    oss << "  " << model.id;
    if (model.id == active_model) oss << " (active)";
    if (!model.caps.credits.empty()) oss << "  " << model.caps.credits;
    oss << "\n";
  }
  if (!active_model.empty() && !active_listed) {
    oss << "  " << active_model << " (active, not listed by this gateway)\n";
  }
  return oss.str();
}

}  // namespace pu::llm
