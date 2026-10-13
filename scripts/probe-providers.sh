#!/usr/bin/env bash
# Collect the reply-side field names a gateway actually sends, so a ResponseShape for a
# new backend is written from a capture rather than from a specification. Raw streams are
# written under $OUT; the summary names only what each stream actually contained.
set -u

OUT="${OUT:-./provider-probes}"
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# Fill these in. A host must not end in a slash; the path is appended verbatim.
# ---------------------------------------------------------------------------
: "${DEEPSEEK_API_KEY:=}"      # https://platform.deepseek.com
: "${NVIDIA_API_KEY:=}"           # https://build.nvidia.com   (key starts with nvapi-)
: "${OLLAMA_API_KEY:=}"        # Ollama Cloud; leave empty for a local daemon
: "${CODEBUDDY_API_KEY:=}"     # optional
: "${OLLAMA_HOST:=https://ollama.com}"
: "${CODEBUDDY_HOST:=https://www.codebuddy.ai}"

DEEPSEEK_MODEL="${DEEPSEEK_MODEL:-deepseek-chat}"
NIM_MODEL="${NIM_MODEL:-deepseek-ai/deepseek-r1}"
OLLAMA_MODEL="${OLLAMA_MODEL:-gpt-oss:120b}"
CODEBUDDY_MODEL="${CODEBUDDY_MODEL:-deepseek-v4-flash}"

PROMPT='Say the single word: pong'

say() { printf '\n== %s ==\n' "$*"; }

# The summary reads JSON, so pick a working interpreter once rather than assuming a name.
# A `python3` that only advertises itself in the Windows Store fails the probe below.
PY=""
for cand in python3 python jq; do
  if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'pass' >/dev/null 2>&1; then
    PY="$cand"; break
  fi
done
[ -z "$PY" ] && echo "note: no python3/python found; the summary will fall back to raw grep." >&2

# An OpenAI-compatible gateway sends no usage object unless it is asked for, so every
# stream probe requests one; without it the usage half of the shape proves nothing.
body() {
  cat <<JSON
{"model":"$1","stream":true,"stream_options":{"include_usage":true},
 "messages":[{"role":"user","content":"$PROMPT"}]}
JSON
}

# POST a stream and keep every byte of it.
collect() {
  local name="$1" url="$2" auth="$3" json="$4"; shift 4
  local raw="$OUT/$name.raw" hdr="$OUT/$name.headers"
  say "$name  ->  $url"
  local args=(-sS -N --max-time 120 -X POST "$url"
              -H 'Content-Type: application/json'
              -H 'Accept: text/event-stream'
              -o "$raw" -w '%{http_code}' -D "$hdr")
  [ "$auth" != "-" ] && args+=(-H "$auth")
  local code
  code=$(curl "${args[@]}" -d "$json" "$@" 2>"$OUT/$name.err")
  echo "http_status: $code"
  echo "bytes:       $(wc -c <"$raw" 2>/dev/null || echo 0)"
  if [ -s "$OUT/$name.err" ]; then echo "curl stderr:"; sed 's/^/  /' "$OUT/$name.err"; fi
  if [ "$code" != "200" ]; then echo "body:"; head -c 600 "$raw"; echo; fi
}

# GET an endpoint and pretty-print when it is JSON.
fetch() {
  local name="$1" url="$2" auth="$3"; shift 3
  say "$name  ->  $url"
  local args=(-sS --max-time 60 "$url" -w '\nhttp_status:%{http_code}\n')
  [ "$auth" != "-" ] && args+=(-H "$auth")
  curl "${args[@]}" "$@" -o "$OUT/$name.body" >"$OUT/$name.meta" 2>&1
  cat "$OUT/$name.meta"
  if command -v jq >/dev/null 2>&1; then
    jq . "$OUT/$name.body" 2>/dev/null | head -80 || head -c 800 "$OUT/$name.body"
  else
    python3 -c 'import json,sys;print(json.dumps(json.load(open(sys.argv[1])),indent=2)[:3000])' \
      "$OUT/$name.body" 2>/dev/null || head -c 800 "$OUT/$name.body"
  fi
  echo
}

say "1. Chat streams (this is where the ResponseShape fields come from)"
[ -n "$DEEPSEEK_API_KEY" ] && collect deepseek-stream \
  "https://api.deepseek.com/chat/completions" "Authorization: Bearer $DEEPSEEK_API_KEY" \
  "$(body "$DEEPSEEK_MODEL")"

[ -n "$NVIDIA_API_KEY" ] && collect nim-stream \
  "https://integrate.api.nvidia.com/v1/chat/completions" "Authorization: Bearer $NVIDIA_API_KEY" \
  "$(body "$NIM_MODEL")"

if [ -n "$OLLAMA_API_KEY" ]; then
  collect ollama-stream "$OLLAMA_HOST/api/chat" "Authorization: Bearer $OLLAMA_API_KEY" \
    "$(cat <<JSON
{"model":"$OLLAMA_MODEL","stream":true,
 "messages":[{"role":"user","content":"$PROMPT"}]}
JSON
)"
else
  collect ollama-local "$OLLAMA_HOST/api/chat" "-" \
    "$(cat <<JSON
{"model":"$OLLAMA_MODEL","stream":true,
 "messages":[{"role":"user","content":"$PROMPT"}]}
JSON
)"
fi

[ -n "$CODEBUDDY_API_KEY" ] && collect codebuddy-stream \
  "$CODEBUDDY_HOST/v1/chat/completions" "Authorization: Bearer $CODEBUDDY_API_KEY" \
  "$(body "$CODEBUDDY_MODEL")"

say "2. Model listings (context window and how a model is named)"
[ -n "$DEEPSEEK_API_KEY" ] && fetch deepseek-models \
  "https://api.deepseek.com/v1/models" "Authorization: Bearer $DEEPSEEK_API_KEY"

[ -n "$NVIDIA_API_KEY" ] && fetch nim-models \
  "https://integrate.api.nvidia.com/v1/models" "Authorization: Bearer $NVIDIA_API_KEY"

if [ -n "$OLLAMA_API_KEY" ]; then
  fetch ollama-tags "$OLLAMA_HOST/api/tags" "Authorization: Bearer $OLLAMA_API_KEY"
  fetch ollama-show "$OLLAMA_HOST/api/show" "Authorization: Bearer $OLLAMA_API_KEY" \
    -H 'Content-Type: application/json' -d "{\"model\":\"$OLLAMA_MODEL\"}"
fi

say "3. Summary - the field names actually present in each stream"
for raw in "$OUT"/*.raw; do
  [ -s "$raw" ] || continue
  echo
  echo "--- $(basename "$raw" .raw) ---"
  if [ -n "$PY" ]; then
    "$PY" - "$raw" <<'PY'
import json, sys

top, choice, piece, usage, nested = set(), set(), set(), set(), {}
frames = 0

def frames_of(path):
    buf = ""
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line == "data: [DONE]":
            continue
        if line.startswith("data:"):
            # A payload split over several `data:` lines is joined before it is read,
            # matching how the reader itself reassembles one frame.
            buf = (buf + "\n" + line[5:].lstrip()) if buf else line[5:].lstrip()
        else:
            buf = (buf + "\n" + line) if buf else line
        try:
            obj = json.loads(buf)
        except Exception:
            continue
        buf = ""
        yield obj

for obj in frames_of(sys.argv[1]):
    if not isinstance(obj, dict):
        continue
    frames += 1
    top |= set(obj)
    u = obj.get("usage")
    if isinstance(u, dict):
        usage |= set(u)
        for k, v in u.items():
            if isinstance(v, dict):
                nested[k] = sorted(v)
    for c in obj.get("choices") or []:
        if not isinstance(c, dict):
            continue
        choice |= set(c)
        for holder in ("delta", "message"):
            p = c.get(holder)
            if isinstance(p, dict):
                piece |= set(p)

print("frames parsed :", frames)
for label, keys in (("top-level", top), ("choice", choice),
                    ("delta/msg", piece), ("usage", usage)):
    print("%-13s : %s" % (label, ", ".join(sorted(keys)) or "(none)"))
for k, names in sorted(nested.items()):
    print("usage.%-7s : %s" % (k, ", ".join(names)))
PY
  else
    # No interpreter: list every distinct key name the stream mentions, which is enough
    # to spot a renamed field even without the structure around it.
    grep -o '"[A-Za-z_][A-Za-z0-9_]*"[[:space:]]*:' "$raw" \
      | tr -d '":' | sort -u | tr '\n' ' '; echo
  fi
done

say "Raw streams are under $OUT/ - attach the .raw files alongside the shape you add."
