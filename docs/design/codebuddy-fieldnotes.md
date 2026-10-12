# CodeBuddy: what was probed

Observed behavior of the CodeBuddy gateway, recorded as it was found. This is **not**
the contract — [codebuddy-cloud-api.md](codebuddy-cloud-api.md) is. Everything here was
established by calling the endpoint with a real account key, so it describes one
account at one point in time and will drift.

Treat a claim here as a lead to re-probe, not as a fact to rely on.

## Which fields are honored

The baseline request plus one difference, measured against a prompt that benefits from
reasoning:

| Field | Result |
| --- | --- |
| `stream: true` | required in practice |
| `temperature`, `max_tokens` | accepted |
| `reasoning_effort: "high"` | **accepted and honoured**. The same prompt answered `ok` with no reasoning at all on the baseline; with this field the stream carried `reasoning_content` fragments (`"We"`, `" need"`, …) |
| `extra_body: {"thinking":{"type":"disabled"}}` | accepted (200), and inert on this gateway: the default request already emits no reasoning for a prompt that benefits from one, so there is nothing for the field to suppress |
| `stream_options: {"include_usage": true}` | accepted (200), and the `usage` object arrives per chunk with or without it |

## The error envelope

A rejected request answers non-200 with an envelope of its own:

```json
{"code":11102,
 "msg":"model [deepseek-v3] service info not found",
 "requestId":"...",
 "displayMsg":{"en":"Model unavailable. Please switch models","zh":"当前模型不可用，请切换模型","zh-hant":"..."},
 "displayTips":{"en":"...","zh":"...","zh-hant":"..."},
 "actions":["SWITCH_MODEL","SUBMIT_FEEDBACK","RETRY"]}
```

`displayMsg` is written for a user and is what should be shown; `msg` names the
specific cause and is what belongs in a log. `code` 11102 covers both an unknown model
and one the account is not entitled to, distinguished only by `msg`:

```
model [claude-opus-4.6] is only available for authorized users
```

## Which models answer

Found by trying ids until one answered. This table rots fastest; it is a record of what
happened to be true, not a list to code against.

Answers 200: `deepseek-v4-flash`, `deepseek-r1`, `glm-5.3-flash`, `glm-5.3`, `glm-5.2`,
`glm-5.1`.

Answers 11102 `service info not found`: `glm-5-flash`, `glm-4.7`, `glm-5`, `glm-5-turbo`,
`glm-4.6v`, and every dated Claude id the client carries.

Ids are matched exactly, so `GLM-5.3-Flash` is 11102. An id that exists without this
account being entitled to it says so instead: `glm-4.7-ioa` answers `model
[glm-4.7-ioa] is only available for authorized users`.

The ids the installed client names, for reference: `deepseek-v3`, `deepseek-v3-0324`,
`deepseek-v4-flash`, `deepseek-v4-flash-202605`, `deepseek-v4-pro`,
`deepseek-v4-pro-202606`, `deepseek-r1`, `deepseek-reasoner`, `claude-3-5-sonnet-20241022`,
`claude-3-7-sonnet-20250219`, `claude-4.5`, `claude-opus-4.6`, `claude-sonnet-4.5`,
`gemini-2.5-flash`, `gemini-3.1-flash`, `glm-4.7`, `glm-4.7-ioa`, `glm-4.6v`, `glm-5`,
`glm-5.1`, `glm-5.2`, `glm-5-turbo`, `hunyuan-t1`, `hunyuan-turbos`. Being named there
does not mean the account may use it: entitlement is the server's answer, and only a
request settles it. Serving a model the client omits shows the reverse also holds, so the
client's list is a hint and not an inventory — `glm-5.3-flash` works without appearing
there at all.

## Still open

- Which headers are actually required, and which are the client's own telemetry.
- Whether `extra_body.thinking` would suppress reasoning if this gateway ever reasoned by
  default: it does not, so the field is inert here rather than wrong.
- Whether a model list can be discovered for an account at all. `/v2/accounts` settles
  identity and entitlement flags but not the model set, so ids are still found by trying
  them.
- What `usage.credit` means for a session, and whether it is worth surfacing.
- Whether `/v2/auth/token/refresh` is needed for keys that expire; a request with a
  valid key needs no refresh.
