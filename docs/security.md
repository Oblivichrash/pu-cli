# Security

What this build admits, what it refuses, and where the line sits. This document owns the
trust boundary; the field names live in
[configuration.md](configuration.md#agentsjson), and a route's shape is in
[web-api.md](web-api.md).

## What the server is not

`pu serve` has no authentication on `/api/*` and `/ws`, and no origin check. A process that
reaches the port can drive a turn, and a turn runs the agent's tools. The default bind is
`127.0.0.1` for that reason: the boundary is the network interface, not a credential.
`PU_SERVE_BIND` and `--bind` move it, and moving it to a routable address hands the tools to
whatever can reach the port.

Two audiences narrow what is reachable over that listener, and every route names the one it
answers in the route table — a row without an audience does not compile:

| Audience | Routes | Narrowed by |
| --- | --- | --- |
| `loopback` | `/api/servers` | The address the request arrived from. The answer is a fact about this machine — a list of what it serves — so a caller from anywhere else gets `403` |
| `anyone` | The rest | Nothing beyond the bind address |

The reason this is a column rather than a check inside the dispatcher is that a check in the
dispatcher has to name a path, and a path it does not name is a route that answers the
network. A route that answered this machine's directories was added that way once: it named
no audience, so by omission it answered the network. The route is gone; the column is what
keeps the next one from being added the same way.

`anyone` is a known gap rather than an oversight, and it is why the bind address is the
control that matters: a caller that reaches the port can read the conversation, drive a turn,
and run the agent's tools. The audience column narrows the routes whose answers are facts
about this machine; it does not authenticate the page.

## The sandbox

`security.sandbox_root` is where every file operation lands: a relative path resolves against
the workspace, and the working directory a command runs in is the root itself, so a `cd` that
escapes it is not a thing the model can express. Unset, the root is the workspace.

`forbidden_patterns` is enforced at the tool execution layer rather than at the model, so a
command that matches is rejected with a JSON error whatever asked for it. `approval_patterns`
does not refuse: a matching command parks until a person answers, which the page shows as a
card above the composer and the terminal shows as a prompt. Both lists are substring matches
on the command text, so a policy is a boundary against what the model was told not to do, not
a defence against a model that encodes its way around a substring.

A policy that is wrong is visible rather than silent: `pu check` and the load both report the
resolved `sandbox_root`, the patterns in force, and whether a level came from the workspace or
the user's file.

## Secrets

- `api_key` is sent as `Authorization: Bearer` and is not written to the session file.
- `${ENV}` in `agents.json` reads an environment variable at load, so a key can stay out of
  the file. An unset variable expands to an empty string rather than failing the load, which
  is worth knowing before it reaches a provider as a blank key — see
  [configuration.md](configuration.md#agentsjson).
- No prompt takes a key. `/backend` refuses a fourth argument rather than adopt a backend
  with a key typed at the chat prompt, because that prompt is echo'd into the scrollback,
  into the terminal's own history, and into anything that captures a screen. A keyed
  endpoint is named in `agents.json`.
- A stored session holds the conversation, which is whatever was said to the model. It does
  not hold the key that was used to reach it.
