// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// What a gateway says about the models it offers, as data.
//
// A capability is a promise, a context window is a claim, and the price coefficient is a number
// to compare two models by — all three are read from a catalogue and then used far from the
// socket that read them, by the table, the agents summary and the listing a person reads. They
// live here, beside `Backend`, because both describe a model rather than the way one is reached.
//
// The listers that fetch a catalogue stay in `llm`, where the network is.
//

#include <string>

namespace pu::model {

struct Capabilities {
  bool supports_tools = false;
  bool supports_images = false;
  bool supports_reasoning = false;
  // Measured: a request that carries no effort is answered 400 `11150`.
  bool only_reasoning = false;
  std::string default_effort;
  int max_input_tokens = 0;
  // The gateway's own price coefficient, the one number deciding between two models that work.
  std::string credits;
};

struct ModelEntry {
  std::string id;
  Capabilities caps;
};

// How a gateway's catalogue is shaped, as the lister that reads it declares: which member
// holds the entries, which key of an entry is its id, and what a model says about itself.
struct CatalogShape {
  std::string path{};
  std::string container;
  std::string id_key;
  std::string nested{};
  std::string supports_tools{};
  std::string supports_images{};
  std::string supports_reasoning{};
  std::string only_reasoning{};
  // One level: the parent object and the field under it.
  std::string effort_parent{};
  std::string effort_field{};
  std::string max_input_tokens{};
  // The window a model declares rather than a capability it promises: read when no capability
  // field named one, because several products state it and none of them promise it.
  std::string context_window{};
  std::string credits{};
};

// How a gateway's catalogue is fetched once its origin is known: where the listing lives, and
// the shape its answer is read with. Exposed so a probe can fetch the same endpoint the lister
// does and diff the bytes against the same names, rather than restating either.
struct ListingTarget {
  std::string url;
  CatalogShape shape;
};

}  // namespace pu::model
