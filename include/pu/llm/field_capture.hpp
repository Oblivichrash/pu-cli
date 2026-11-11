// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pu/config/backend.hpp"

namespace pu::llm {

// The field names one gateway actually sent, collected from a raw capture rather than
// from a specification. A name missing from a level means the gateway did not send it;
// a name here that no shape field reads is the one a new adapter must add.
struct FieldCapture {
  std::string gateway;
  std::string host;
  std::string model;

  unsigned http_status = 0;
  size_t bytes = 0;
  size_t frames = 0;
  bool stream_parsed = false;

  // Level -> the distinct key names that level carried, sorted.
  std::map<std::string, std::set<std::string>> keys;

  std::string error;

  // Whether the probe found what it went for. A reply that parsed is a finding even when it
  // carries names no shape reads: that is a gateway answering a shape this build does not
  // know, which is what a capture is run to learn. Only a reply that never parsed means the
  // run told nobody anything.
  bool Succeeded() const { return stream_parsed; }
};

// Sends one single-token prompt as a stream, keeps every byte of the reply, and reads
// the field names back out of it. `raw_path`, when set, receives the unmodified stream.
// Uses the backend's own host, model and key, so nothing is retyped for a probe.
FieldCapture CaptureFields(const config::Backend& backend, pu::http::HttpClient& http,
                        const std::string& prompt, const std::optional<std::string>& raw_path);

// The summary printed for a capture: what each level named.
std::string FormatFieldCapture(const FieldCapture& report);

// The same questions asked of a model listing rather than of a stream: the keys the answer
// carried at each level, and which of the names the catalogue shape reads it never sent.
// Kept apart from FieldCapture because a listing is one object, not a stream of frames.
struct ListingReport {
  std::string gateway;
  std::string url;
  unsigned http_status = 0;
  size_t bytes = 0;
  size_t entries = 0;
  bool parsed = false;

  // Level -> the distinct key names that level carried, sorted.
  std::map<std::string, std::set<std::string>> keys;

  // Names the shape reads that the answer never carried. Empty names in the shape are not
  // asked about: a gateway that has no such concept is not a gateway that dropped it.
  std::vector<std::string> shape_fields_absent;

  std::string error;

  // The same question as `FieldCapture::Succeeded`, asked of an object rather than a stream:
  // an answer that parsed is a finding whatever it left out.
  bool Succeeded() const { return parsed; }
};

// Reads the listing the gateway's own catalogue endpoint answers and collects what it named.
// Uses the backend's host and key, so nothing is retyped for a probe.
ListingReport ProbeListing(const config::Backend& backend, pu::http::HttpClient& http);

std::string FormatListingReport(const ListingReport& report);

}  // namespace pu::llm
