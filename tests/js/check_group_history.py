#!/usr/bin/env python3
"""Run the transcript fold's cases without a JavaScript interpreter.

This is a stand-in for ``tests/js/check-group-history.mjs`` on a machine with no node. It
reads the same case file and reimplements the fold exactly as ``web/history.js`` writes it,
so the cases can be authored here and are still checked by the script on CI. What it does
NOT prove is that the JavaScript itself parses or runs; that is the script's job, and this
file says so when it passes.
"""

import json
import pathlib
import sys

BLOCK = {"THINKING": "thinking", "TOOL_CALL": "tool_call", "TEXT": "text"}


def parse_tool_arguments(raw):
    if isinstance(raw, str):
        try:
            return json.loads(raw)
        except ValueError:
            return raw
    return None if raw is None else raw


def call_status(statuses, index):
    stored = statuses[index] if index < len(statuses) else None
    if stored in ("pending", "interrupted"):
        return stored
    return "done"


def thinking_text(msg):
    reasoning = msg.get("reasoning")
    if not isinstance(reasoning, list) or not reasoning:
        return None
    return "".join(block.get("text") or "" for block in reasoning)


def append_at_end(blocks, kind, make):
    if blocks and blocks[-1]["type"] == kind:
        return blocks[-1]
    block = make()
    blocks.append(block)
    return block


def tool_call_block(call, status):
    fn = call.get("function") or {}
    return {
        "type": BLOCK["TOOL_CALL"],
        "id": call.get("id") or "unknown",
        "name": fn.get("name") or "unknown",
        "args": parse_tool_arguments(fn.get("arguments")),
        "output": "",
        "error": "",
        "status": status,
        "collapsed": True,
    }


def apply_tool_result(blocks, msg):
    for block in blocks:
        if block["type"] == BLOCK["TOOL_CALL"] and block["id"] == msg.get("tool_call_id"):
            block["output"] = msg["output"] if isinstance(msg.get("output"), str) else (msg.get("content") or "")
            block["error"] = msg.get("error") or ""
            block["status"] = "done"
            return


def group_history(history):
    turns = []
    reply = [None]  # a box, so the closures below can rebind it

    def close_reply():
        if reply[0]:
            turns.append(reply[0])
        reply[0] = None

    def current_reply():
        if not reply[0]:
            reply[0] = {"role": "assistant", "blocks": []}
        return reply[0]

    for msg in history:
        content = msg.get("content") or ""
        role = msg.get("role")

        if role == "system":
            close_reply()
            turns.append({"role": "system", "text": content})
        elif role == "user":
            close_reply()
            turns.append({"role": "user", "text": content, "turn": msg.get("id")})
        elif role == "tool":
            apply_tool_result(current_reply()["blocks"], msg)
        elif role == "assistant":
            bubble = current_reply()
            text = thinking_text(msg)
            if text:
                append_at_end(
                    bubble["blocks"],
                    BLOCK["THINKING"],
                    lambda: {"type": BLOCK["THINKING"], "content": "", "collapsed": True},
                )["content"] += text

            calls = msg.get("tool_calls") if isinstance(msg.get("tool_calls"), list) else []
            statuses = msg.get("tool_call_status") if isinstance(msg.get("tool_call_status"), list) else []
            for index, call in enumerate(calls):
                bubble["blocks"].append(tool_call_block(call, call_status(statuses, index)))

            if content:
                append_at_end(
                    bubble["blocks"], BLOCK["TEXT"], lambda: {"type": BLOCK["TEXT"], "content": ""}
                )["content"] += content

    close_reply()
    return turns


def canonical(value):
    if isinstance(value, list):
        return [canonical(item) for item in value]
    if isinstance(value, dict):
        return {key: canonical(value[key]) for key in sorted(value)}
    return value


def main():
    root = pathlib.Path(__file__).resolve().parent.parent.parent
    cases = json.loads((root / "tests" / "fixtures" / "group_history.json").read_text())["cases"]

    failed = 0
    for case in cases:
        actual = canonical(group_history(case["history"]))
        expected = canonical(case["expected"])
        if actual == expected:
            print(f"ok   {case['name']}")
            continue
        failed += 1
        print(f"FAIL {case['name']}")
        print(f"  expected {json.dumps(expected, sort_keys=True)}")
        print(f"  actual   {json.dumps(actual, sort_keys=True)}")

    if failed:
        print(f"\n{failed} of {len(cases)} case(s) failed")
        return 1

    print(f"\n{len(cases)} case(s) passed")
    print("(checked by this stand-in, which does not run the JavaScript itself;")
    print(" tests/js/check-group-history.mjs is what runs it where node exists)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
