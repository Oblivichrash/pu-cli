// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/probe.hpp"

#include "pu/core/base.hpp"
#include "pu/core/sse.hpp"
#include "pu/llm/openai_protocol.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace pu::llm {

namespace {

// A shape worth checking against only exists on the OpenAI-format gateways; Ollama's reply
// is a different wire format with no ResponseShape to diff. Asked of the protocol rather
// than of a list of ids, so a product added to the table is covered by being one.
std::optional<llm::ResponseShape> ShapeFor(config::GatewayId id) {
  if (config::GatewayForId(id).protocol == "openai") return llm::ResponseShape{};
  return std::nullopt;
}

bool SpeaksOllama(config::GatewayId id) {
  return config::GatewayForId(id).protocol == "ollama";
}

// The gateway's own headers plus the bearer, which is what both probes send. A gateway that
// names none still gets the content type a POST needs.
std::vector<std::string> ProbeHeaders(const config::Backend& backend,
                                     const std::vector<std::string>& extra = {}) {
  std::vector<std::string> headers = extra;
  if (headers.empty()) headers = config::GatewayForId(backend.gateway).listing_headers();
  if (std::none_of(headers.begin(), headers.end(), [](const std::string& h) {
        return h.rfind("Content-Type:", 0) == 0;
      })) {
    headers.push_back("Content-Type: application/json");
  }
  const std::string bearer = http::BearerHeader(backend.api_key.value_or(""));
  if (!bearer.empty()) headers.push_back(bearer);
  return headers;
}

std::string ProbeBody(const config::Backend& backend, const std::string& prompt) {
  if (SpeaksOllama(backend.gateway)) {
    boost::json::value req = {
        {"model", backend.model},
        {"stream", true},
        {"messages", boost::json::array{boost::json::value{{"role", "user"}, {"content", prompt}}}},
    };
    return boost::json::serialize(req);
  }
  boost::json::value req = {
      {"model", backend.model},
      {"stream", true},
      {"stream_options", {{"include_usage", true}}},
      {"messages",
       boost::json::array{boost::json::value{{"role", "user"}, {"content", prompt}}}},
  };
  return boost::json::serialize(req);
}

// Reads a capture with the parser the live stream uses, so a probe reports the frames the
// provider would have seen rather than its own third reading of the format. The bytes are
// whole here, so everything arrives in one feed and the close terminates the last line.
template <typename Fn>
void ForEachFrame(const std::string& raw, Fn&& on_frame) {
  core::SseParser parser([&](const core::SseEvent& event) {
    if (event.data.empty() || event.data == "[DONE]") return;
    boost::system::error_code ec;
    const boost::json::value parsed = boost::json::parse(event.data, ec);
    if (!ec) on_frame(parsed);
  });
  // A capture that carries no `data:` line is newline-delimited JSON, and the parser delivers
  // each of its lines as an event of its own.
  parser.SetBareLinesAreEvents(true);
  parser.Feed(raw);
  parser.Finish();
}

void CollectKeys(const boost::json::value& j, const std::string& level,
                 std::map<std::string, std::set<std::string>>& keys) {
  const boost::json::object* obj = j.if_object();
  if (obj == nullptr) return;
  for (const auto& member : *obj) {
    keys[level].insert(std::string(member.key()));
  }
}

void CollectOpenAIShape(const boost::json::value& j,
                        std::map<std::string, std::set<std::string>>& keys) {
  CollectKeys(j, "top-level", keys);
  const boost::json::object* obj = j.if_object();
  if (obj == nullptr) return;
  if (const auto it = obj->find("choices"); it != obj->end() && it->value().is_array() &&
                                              !it->value().as_array().empty()) {
    const boost::json::object& choice = it->value().as_array().at(0).as_object();
    for (const auto& member : choice) keys["choice"].insert(std::string(member.key()));
    if (const auto delta = choice.find("delta");
        delta != choice.end() && delta->value().is_object()) {
      for (const auto& member : delta->value().as_object()) {
        keys["delta"].insert(std::string(member.key()));
      }
    }
  }
  if (const auto usage = obj->find("usage");
      usage != obj->end() && usage->value().is_object()) {
    const boost::json::object& u = usage->value().as_object();
    for (const auto& member : u) {
      keys["usage"].insert(std::string(member.key()));
      if (member.value().is_object()) {
        for (const auto& nested : member.value().as_object()) {
          keys["usage." + std::string(member.key())].insert(std::string(nested.key()));
        }
      }
    }
  }
}

}  // namespace

ProbeReport ProbeStream(const config::Backend& backend, pu::http::HttpClient& http,
                        const std::string& prompt, const std::optional<std::string>& raw_path) {
  ProbeReport report;
  report.gateway = config::GatewayName(backend.gateway);
  report.host = backend.host;
  report.model = backend.model;

  std::string raw;
  http.PostStream(backend.host + std::string(config::GatewayEndpointPath(backend.gateway)),
                  ProbeBody(backend, prompt),
                  ProbeHeaders(backend),
                  [&](char* ptr, size_t total) -> size_t {
                    raw.append(ptr, total);
                    return total;
                  });
  report.bytes = raw.size();

  if (raw_path && !raw_path->empty()) {
    std::ofstream out(*raw_path, std::ios::binary);
    out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
  }

  ForEachFrame(raw, [&](const boost::json::value& frame) {
    ++report.frames;
    if (SpeaksOllama(backend.gateway)) {
      CollectKeys(frame, "top-level", report.keys);
      if (const boost::json::object* obj = frame.if_object()) {
        if (const auto msg = obj->find("message"); msg != obj->end() && msg->value().is_object()) {
          for (const auto& member : msg->value().as_object()) {
            report.keys["message"].insert(std::string(member.key()));
          }
        }
      }
    } else {
      CollectOpenAIShape(frame, report.keys);
    }
  });
  report.stream_parsed = report.frames > 0;

  // An empty capture says the request failed before any shape was sent; diffing it against
  // the defaults would report every field absent and say nothing useful.
  if (report.stream_parsed) {
    if (const std::optional<llm::ResponseShape> shape = ShapeFor(backend.gateway)) {
      const auto present = [&report](std::string_view field) {
        for (const auto& [level, names] : report.keys) {
          if (std::any_of(names.begin(), names.end(),
                          [field](const std::string& name) { return name == field; })) {
            return true;
          }
        }
        return false;
      };
      report.shape_fields_absent = llm::AbsentShapeFields(*shape, present, /*required_only=*/false);
    }
  }

  return report;
}

namespace {

// Collects the keys of one listing entry, and of the nested objects a shape reads through.
// A level is named by the path that reaches it, so two entries answering the same shape
// land in the same set rather than one overwriting the other.
void CollectListingKeys(const boost::json::value& entry, std::map<std::string, std::set<std::string>>& keys) {
  const boost::json::object* obj = entry.if_object();
  if (obj == nullptr) return;
  for (const auto& member : *obj) {
    keys["entry"].insert(std::string(member.key()));
    if (member.value().is_object()) {
      for (const auto& nested : member.value().as_object()) {
        keys["entry." + std::string(member.key())].insert(std::string(nested.key()));
      }
    }
  }
}

}  // namespace

ListingReport ProbeListing(const config::Backend& backend, pu::http::HttpClient& http) {
  ListingReport report;
  report.gateway = config::GatewayName(backend.gateway);

  const std::optional<ListingTarget> target = ListingTargetFor(
      config::GatewayForId(backend.gateway).protocol, report.gateway, backend.host);
  if (!target) {
    report.error = "no listing is known for gateway " + report.gateway;
    return report;
  }
  report.url = target->url;

  const http::Response response = http.Get(target->url, ProbeHeaders(backend));
  report.http_status = response.status;
  report.bytes = response.body.size();

  boost::json::value parsed;
  try {
    parsed = boost::json::parse(response.body);
  } catch (const boost::system::system_error&) {
    report.error = "the answer does not parse as JSON";
    return report;
  }
  report.parsed = true;

  // Every top-level key, then the first entry of whatever container the shape names, at the
  // level the shape reads it from. One entry answers the question: a listing repeats a shape.
  const boost::json::object* top = parsed.if_object();
  if (top != nullptr) {
    for (const auto& member : *top) {
      report.keys["top-level"].insert(std::string(member.key()));
      if (member.value().is_object()) {
        for (const auto& nested : member.value().as_object()) {
          report.keys["top-level." + std::string(member.key())].insert(std::string(nested.key()));
        }
      }
    }
  }

  // The container the shape names, wherever the gateway puts it: CodeBuddy wraps everything
  // in `data` and then nests two deep (`data.agents[].models[]`), the others carry one array
  // at the top. Following the shape rather than guessing keeps this the same walk the lister
  // does, so a listing that reads is a listing that probes.
  const boost::json::array* entries = nullptr;
  const boost::json::object* search = top;
  if (top != nullptr) {
    if (const auto data = top->find("data");
        data != top->end() && data->value().is_object()) {
      search = &data->value().as_object();
    }
  }
  if (search != nullptr) {
    // Both arrays a nested shape names: the agents whose `models` holds the ids, and the
    // catalogue that carries the capabilities beside it. A flat shape names the same array
    // twice, which the sets fold into one answer.
    for (const std::string& key : {target->shape.container, target->shape.id_key}) {
      if (key.empty()) continue;
      const auto it = search->find(key);
      if (it == search->end() || !it->value().is_array() || it->value().as_array().empty()) {
        continue;
      }
      entries = &it->value().as_array();
      report.entries = entries->size();
      // Every entry, not the first: `onlyReasoning` is carried only by the models that
      // reason, so one entry would report a field this gateway does send as missing.
      for (const auto& entry : *entries) CollectListingKeys(entry, report.keys);
    }
  }

  // The shape's own names, asked of everything the answer carried.
  const auto present = [&report](const std::string& field) {
    for (const auto& [level, names] : report.keys) {
      if (names.count(field) != 0) return true;
    }
    return false;
  };
  const auto expect = [&](const std::string& field) {
    if (!field.empty() && !present(field)) report.shape_fields_absent.emplace_back(field);
  };
  expect(target->shape.id_key);
  expect(target->shape.supports_tools);
  expect(target->shape.supports_images);
  expect(target->shape.supports_reasoning);
  expect(target->shape.only_reasoning);
  expect(target->shape.effort_field);
  expect(target->shape.max_input_tokens);
  expect(target->shape.credits);
  return report;
}

std::string FormatListingReport(const ListingReport& report) {
  std::ostringstream out;
  out << "gateway: " << report.gateway << "  listing: " << report.url << "\n";
  if (!report.error.empty()) {
    out << "http " << report.http_status << "  " << report.error << "\n";
    return out.str();
  }
  out << "http " << report.http_status << "  bytes: " << report.bytes
      << "  entries: " << report.entries << "\n";

  for (const auto& [level, names] : report.keys) {
    out << level << " : ";
    bool first = true;
    for (const std::string& name : names) {
      if (!first) out << ", ";
      out << name;
      first = false;
    }
    out << "\n";
  }

  if (!report.shape_fields_absent.empty()) {
    out << "\nfields the catalogue shape reads that this listing never sent:\n";
    for (const std::string& field : report.shape_fields_absent) {
      out << "  " << field << "\n";
    }
    out << "a shape for this gateway names these differently, or drops them\n";
  }
  return out.str();
}

std::string FormatProbeReport(const ProbeReport& report) {
  std::ostringstream out;
  out << "gateway: " << report.gateway << "  host: " << report.host
      << "  model: " << report.model << "\n";
  out << "bytes: " << report.bytes << "  frames parsed: " << report.frames << "\n";

  if (!report.stream_parsed) {
    out << "\nNo frame parsed. Raw stream follows:\n" << report.error << "\n";
    return out.str();
  }

  for (const auto& [level, names] : report.keys) {
    out << level << " : ";
    bool first = true;
    for (const std::string& name : names) {
      if (!first) out << ", ";
      out << name;
      first = false;
    }
    out << "\n";
  }

  if (!report.shape_fields_absent.empty()) {
    out << "\nfields the built-in ResponseShape reads that this stream never sent:\n";
    for (const std::string& field : report.shape_fields_absent) {
      out << "  " << field << "\n";
    }
    out << "an adapter for this gateway names these differently, or drops them\n";
  }
  return out.str();
}

}  // namespace pu::llm
