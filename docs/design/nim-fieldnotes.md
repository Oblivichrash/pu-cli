# NVIDIA NIM: what was probed

Observed behavior of the NVIDIA NIM endpoint at `integrate.api.nvidia.com`, recorded
as it was found. Everything here was established by calling the endpoint with a real
account key, so it describes one account at one point in time and will drift. Treat a
claim here as a lead to re-probe, not as a fact to rely on.

## What answers

`GET /v1/models` answers `200` with 80 models whose keys are only `created`, `id`,
`object`, and `owned_by`, and `GET /v1/metadata` answers `404 page not found`. This is
the state `providers.md` records, re-confirmed.

## Which chat models answer for this account

The catalog is not what the account can call. Probed on 2026-10-14:

| Model | Result |
| --- | --- |
| `nvidia/nemotron-3-super-120b-a12b` | streams |
| `nvidia/nemotron-3-ultra-550b-a55b` | streams |
| `openai/gpt-oss-20b` | streams |
| `deepseek-ai/deepseek-v4.1-flash` | hangs: connection accepted, no byte for 90 s, stream and non-stream alike |
| `moonshotai/kimi-k3` | hangs the same way at 30 s |
| `z-ai/glm-5.3-flash` | hangs the same way at 30 s |
| most other ids tried (gemma, granite, jamba, phi, nemotron-nano, kimi-k2.6, palmyra, mistral, llama, yi-large…) | `404` JSON naming a function id and the account |
| `meta/llama-3.1-8b-instruct` | `410` with an end-of-life date |
| `qwen/qwen2.5-7b-instruct`, `deepseek-ai/deepseek-r1` | plain-text `404 page not found`, not JSON |

The hang and the plain-text 404 have no machine-readable failure to read; a client can
only time out or treat the body as text.

## The stream, from a nemotron-3-super capture

The capture this section describes is kept as
[`tests/fixtures/nim-nemotron3-super.sse`](../../tests/fixtures/nim-nemotron3-super.sse), so a
reader can check a claim here against the bytes rather than re-probing. Nothing reads it in a
test: the `nim` gateway speaks the `openai` protocol, so the parser is exercised by the OpenAI
fixtures and this file is evidence rather than a fixture.

Standard OpenAI-compatible SSE: `data: ` lines, blank line between them, ended by
`data: [DONE]`. Each chunk is a `chat.completion.chunk` with a stable per-reply `id`.

- Reasoning arrives first, as `delta.reasoning_content` fragments, then the answer as
  `delta.content`. One chunk carries both the first `content` and
  `finish_reason: "stop"`.
- Every non-final chunk carries `"usage": null`; a final chunk with an empty
  `choices` array carries the usage: `prompt_tokens`, `completion_tokens`,
  `total_tokens`, and `*_tokens_details` whose `reasoning_tokens` names the thinking
  spend. `stream_options: {"include_usage": true}` was set on the request; whether
  usage arrives without it was not probed.
- `prompt_tokens_details.cached_tokens` was `0` on the one capture.

Nothing here was read by the client beyond the fields `providers.md` already names for
OpenAI-compatible backends, so no contract document changes.
