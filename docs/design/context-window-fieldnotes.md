# Which backend reports a context window
#
# None of these do, and each was asked rather than assumed. What follows is what the
# endpoints answered on the day, kept here because these are observations that drift
# rather than a contract anything can rely on.

| Backend | Endpoint | Answer |
| --- | --- | --- |
| DeepSeek | `GET /v1/models` | `context_window: 1048576` on each model |
| Ollama Cloud | `POST /api/show` | a `model_info` key ending in `context_length` |
| Ollama Cloud | `GET /api/ps` | `401 unauthorized`, so the loaded-model window is unreachable |
| NVIDIA NIM | `GET /v1/models` | 80 models, keys are only `created`, `id`, `object`, `owned_by` |
| NVIDIA NIM | `GET /v1/metadata` | `404 page not found` |
| CodeBuddy | `GET /v2/models`, `/v3/config` | `models` is `null`; the endpoint answers 200 and names no models |

## Ollama's key names the model family

The prefix in `model_info` varies with the architecture, so a reader has to match the
suffix, not a fixed name. Measured across five models:

```
gpt-oss:20b        gptoss.context_length         131072
gpt-oss:120b       gptoss.context_length         131072
nemotron-3-super   nemotron_h_moe.context_length 262144
gemma4:31b         gemma4.context_length          262144
nemotron-3-ultra   .context_length                262144
```

The last one has no prefix at all, so a lookup that names `gptoss.` fails on it.

## CodeBuddy truncates instead of refusing

A request carrying two million characters was answered `200` with a normal reply. The
counts in that reply are what shows what happened:

```
prompt_tokens:            250004
prompt_cache_hit_tokens:  249856
prompt_cache_miss_tokens:     148
```

249,856 plus 148 is 250,004, so the cache stops at 250,000 and the rest fell outside
it. Two readings of the same reply put the ceiling at 250,000 tokens, and the gateway
never said so — there is no error to catch and no field to read.

**What follows from this**: a request that outgrows CodeBuddy's window is answered as
though it fit, so nothing on the client side can detect it from the status alone. That
number is a record of one measurement, not a limit to code against.
