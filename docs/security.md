# Security

What this build admits, what it refuses, and where the line sits. This document owns the
trust boundary; the field names live in
[configuration.md](configuration.md#agentsjson), and a route's shape is in
[web-api.md](web-api.md).

## What the server is not

`pu serve` has no authentication on `/api/*` and `/ws`, and no origin check. A process that
reaches the port can drive a turn, and a turn runs the agent's tools. The default bind is
`127.0.0.1` for that reason: the boundary is the network interface, not a credential.
`PU_SERVE_HOST` and `--host` move it, and moving it to a routable address hands the tools to
whatever can reach the port.

Two things narrow what is reachable over that listener:

| Route | Narrowed by |
| --- | --- |
| `/api/servers`, `/api/board` | Loopback only. Both carry facts the machine knows — a filesystem path per server, and the trail of who asked whom for what — so a non-loopback peer gets `403` |
| `/api/peer/message` | A bearer token compared in constant time, and a `from` that must be in `peers[]` |

Everything else answers any caller that reaches it. That is a known gap rather than an
oversight, and it is why the bind address is the control that matters.

## The peer endpoint

The peer endpoint is the one surface that lets a remote conversation drive a local model, so
it is authenticated while the rest of the HTTP API is not.

| Control | Effect |
| --- | --- |
| `Authorization: Bearer <token>` | Required; compared in constant time |
| Peer allowlist (`peers[]`) | An undeclared `from` is refused before any turn starts |
| Loopback binding | The listener is not exposed beyond the host by default |

`PU_PEER_TOKEN` overrides the token in `peers.json`, so the secret can stay out of the file. A
server with no `peers.json` does not mount the endpoint at all — the route reports that peer
messaging is not configured, and the tool is never registered.

Both sides must agree on the token and on each other's names. A peer entry without a name and
a `url` is skipped with a warning rather than trusted.

The rest of what a peer may do — which agent it may reach, how a handed-on job behaves, and
why a peer cannot cancel one — is in [peers-and-board.md](peers-and-board.md).

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
- `PU_PEER_TOKEN` does the same for the peer token.
- A stored session holds the conversation, which is whatever was said to the model. It does
  not hold the key that was used to reach it.
