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

The candidate set a sweep needs comes from `GET {base}/v3/config`, which answers the
account's whole catalogue at `data.models[].id` and the agent's usable subset at
`data.agents[].models[]` (the shape is in
[codebuddy-cloud-api.md](codebuddy-cloud-api.md#2-model-listing)). One call with
`Authorization` and the client's `User-Agent` returned 34 ids in the first list and 19
in the second, so the union the early sweep needed is one request, not a probe loop.
Which of the two is honest about entitlement is unanswered: the wider list carries
`hunyuan-image-alpha`, which a chat turn cannot name, so listing is still not the same
as being allowed to run, and only a request settles that.

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

## Bases the client carries

The installed client carries two bases: `https://copilot.tencent.com/v2`, which the
code's `DefaultHostFor` names, and `staging-copilot.tencent.com/v2`. Only the first was
probed, and only the first is reachable without a staging credential, so the second is
recorded here rather than in the contract. Whether it answers the same way is unprobed.

## Settled by reading the client's generator

The three per-call correlation headers — `X-Request-ID`, `X-Conversation-Request-ID`,
`X-Conversation-Message-ID` — carry a uuid with its dashes removed, not a second id
format, which is why they are 32 characters where `X-Conversation-ID` and `X-User-Id` are
36. Whether the server would accept the dashed form was not probed; the stripped form is
what every captured request used, so that is what is sent.

## Settled by dropping headers one at a time

`Authorization` is the only required header. Each of the sixteen others was dropped on
its own and the gateway still answered 200; dropping every one of them together did too,
and `GET /v2/accounts` answers with no header but the key. The set is sent to match the
installed client's request byte for byte, not because the server reads it.

This does not make the set removable. A gateway may require a header it does not yet
check, and the client's own request is the safest thing to imitate; the finding is that
the client is not what makes the calls succeed.

## Corrected by a later probe

Claims made here from an early run that did not survive a second look.

**The chunk `id` does not group a conversation.** An early run saw one `id` across five
requests under a shared `X-Conversation-ID`; re-running with three turns produced three
different ids. The early reading was of a single turn's repeated chunks, not of several
turns. Nothing in the response echoes the conversation back.

**The server does not store a conversation.** A turn that established a rule ("end every
reply with ZQ7") was followed by a turn sending **no history** under the same
conversation id; the rule was gone from the answer. Since every request already carries
the whole message array, this changes nothing the client must do — but it does mean a
stable `X-Conversation-ID` buys grouping for the server's own records, not memory.

**`usage.credit` is the call's cost, not a balance** — it read `0` for a one-token answer
and `0.05` for a 203-token one, so it scales with the work rather than reporting an
account's remaining budget.

## Still open

- Whether `extra_body.thinking` would suppress reasoning if this gateway ever reasoned by
  default: it does not, so the field is inert here rather than wrong.
- Whether a model list can be discovered for an account at all. `/v2/accounts` settles
  identity and entitlement flags but not the model set, so ids are still found by trying
  them.
- Whether `/v2/auth/token/refresh` is needed for keys that expire; a request with a
  valid key needs no refresh.
