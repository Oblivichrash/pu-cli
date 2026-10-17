// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/probe.hpp"

#include "pu/core/base.hpp"
#include "pu/llm/openai_protocol.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace pu::llm {

namespace {

// A shape worth checking against only exists on the OpenAI-format gateways; Ollama's
// reply is a different wire format with no ResponseShape to diff.
std::optional<llm::ResponseShape> ShapeFor(config::GatewayId id) {
  if (id == config::GatewayId::kOpenAI || id == config::GatewayId::kCodeBuddy) {
    return llm::ResponseShape{};
  }
  return std::nullopt;
}

std::vector<std::string> ProbeHeaders(const config::Backend& backend) {
  std::vector<std::string> headers = {"Content-Type: application/json"};
  const std::string bearer = http::BearerHeader(backend.api_key.value_or(""));
  if (!bearer.empty()) headers.push_back(bearer);
  return headers;
}

std::string ProbeBody(const config::Backend& backend, const std::string& prompt) {
  if (backend.gateway == config::GatewayId::kOllama) {
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

// Frames the way the reader itself reassembles them. An SSE event runs from one blank
// line to the next; its `data:` lines are fragments of one payload and join directly. A
// line that carries no `data:` prefix is Ollama's reply, which is newline-delimited JSON
// and is a frame in itself.
template <typename Fn>
void ForEachFrame(const std::string& raw, Fn&& on_frame) {
  std::string pending;
  auto flush = [&] {
    if (pending.empty()) return;
    boost::system::error_code ec;
    const boost::json::value parsed = boost::json::parse(pending, ec);
    if (!ec) on_frame(parsed);
    pending.clear();
  };

  for (size_t start = 0; start < raw.size();) {
    const size_t end = raw.find('\n', start);
    const std::string_view line(raw.data() + start,
                                end == std::string::npos ? raw.size() - start : end - start);
    start = end == std::string::npos ? raw.size() : end + 1;

    if (line.empty() || line == "data: [DONE]") {
      flush();
      continue;
    }
    if (line.rfind("data:", 0) == 0) {
      std::string_view payload = line.substr(5);
      if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
      pending.append(payload);
      continue;
    }
    // A fragment that arrives on its own line parses on its own line, or fails the same
    // way a truncated one would.
    boost::system::error_code ec;
    const boost::json::value parsed = boost::json::parse(line, ec);
    if (!ec) on_frame(parsed);
  }
  flush();
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
    if (backend.gateway == config::GatewayId::kOllama) {
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

  // An empty capture says the request failed before any shape was sent; diffing it
  // against the defaults would report every field absent and say nothing useful.
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
    const auto absent = [&](std::string_view field) {
      if (!present(field)) report.shape_fields_absent.emplace_back(field);
    };
    absent(shape->reasoning_field);
    absent(shape->refusal_field);
    absent(shape->usage_field);
    absent(shape->prompt_tokens_field);
    absent(shape->completion_tokens_field);
    absent(shape->total_tokens_field);
    absent(shape->finish_reason_field);
    // The cached count nests on one gateway and sits at the top level on another; either
    // name satisfies the shape, so only a stream carrying neither is reported.
    if (!present(shape->cached_tokens_nested_field) && !present(shape->cached_tokens_top_level_field)) {
      report.shape_fields_absent.emplace_back(shape->cached_tokens_nested_field);
    }
  }
  }

  return report;
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
