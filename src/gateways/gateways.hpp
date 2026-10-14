// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>

#include "pu/config/gateway.hpp"

namespace pu::config {

// The Ollama gate: NDJSON at `{host}/api/chat`, a catalogue at `{host}/api/tags`.
const Gateway& OllamaGateway();

// The OpenAI gate: SSE at `{host}/chat/completions`, a catalogue at `{host}/models`.
const Gateway& OpenAIGateway();

// The CodeBuddy gate: the OpenAI protocol at the CodeBuddy host, with the client's header
// set and a catalogue read from the origin's /v3/config.
const Gateway& CodeBuddyGateway();

}  // namespace pu::config
