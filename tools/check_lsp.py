#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Smoke test for `alcy lsp`.

The language server is a process an editor talks to over a pipe, so the
only honest check runs the built binary through a session: the framing,
the initialize handshake, the diagnostics a didOpen publishes, and the
edit a formatting request returns. The script drives one session, parses
the framed responses, and asserts what each request answered.

The session opens an unformatted but valid program and a broken one, so
one run covers the empty-diagnostics case and the reported case, and
asks the formatter for the edit that canonicalizes the first.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

from utils.env import run_environment
from utils.paths import project_root_dir

MAIN_URI = "file:///main.al"
BROKEN_URI = "file:///broken.al"

# Lint-clean but not canonical: the diagnostics answer is empty while
# the formatter still has a rewrite to return, so one buffer drives both
# assertions.
UNFORMATTED = "struct  Point{x:i32}"
BROKEN = "fn main( -> i32 {\n}\n"


def alcy_binary(build_subdir: str) -> Path:
    alcy = project_root_dir / "out" / build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    return alcy


def frame(message: dict) -> bytes:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    return header + payload


def unframe(data: bytes) -> list[dict]:
    messages = []
    while data:
        split = data.find(b"\r\n\r\n")
        if split < 0:
            break
        length = 0
        for line in data[:split].decode("ascii").split("\r\n"):
            name, _, value = line.partition(":")
            if name.strip().lower() == "content-length":
                length = int(value.strip())
        start = split + 4
        messages.append(json.loads(data[start : start + length].decode("utf-8")))
        data = data[start + length :]
    return messages


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the alcy lsp smoke test.")
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary",
    )
    args = parser.parse_args()

    alcy = alcy_binary(args.build_subdir)
    if not alcy.is_file():
        print(f"alcy binary not found in out/{args.build_subdir}/")
        return 1

    session = [
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"processId": None, "rootUri": None, "capabilities": {}},
        },
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": MAIN_URI,
                    "languageId": "alcy",
                    "version": 1,
                    "text": UNFORMATTED,
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": BROKEN_URI,
                    "languageId": "alcy",
                    "version": 1,
                    "text": BROKEN,
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "textDocument/formatting",
            "params": {
                "textDocument": {"uri": MAIN_URI},
                "options": {"tabSize": 2, "insertSpaces": True},
            },
        },
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit"},
    ]

    proc = subprocess.run(
        [str(alcy), "lsp"],
        input=b"".join(frame(message) for message in session),
        capture_output=True,
        env=run_environment(),
        timeout=300,
    )
    if proc.returncode != 0:
        print(f"alcy lsp exited {proc.returncode}")
        print(proc.stderr.decode("utf-8", "replace"))
        return 1

    responses = unframe(proc.stdout)
    by_id = {message["id"]: message for message in responses if "id" in message}
    published = [
        message["params"]
        for message in responses
        if message.get("method") == "textDocument/publishDiagnostics"
    ]

    def fail(reason: str) -> int:
        print(f"lsp smoke: {reason}")
        print(json.dumps(responses, indent=2))
        return 1

    initialize = by_id.get(1, {}).get("result", {})
    capabilities = initialize.get("capabilities", {})
    if capabilities.get("documentFormattingProvider") is not True:
        return fail("initialize did not advertise documentFormattingProvider")
    if initialize.get("serverInfo", {}).get("name") != "alcy-lsp":
        return fail("initialize did not report the server name")

    main_diagnostics = next(
        (entry["diagnostics"] for entry in published if entry["uri"] == MAIN_URI),
        None,
    )
    if main_diagnostics != []:
        return fail(f"a valid buffer reported {main_diagnostics!r}")

    broken_diagnostics = next(
        (entry["diagnostics"] for entry in published if entry["uri"] == BROKEN_URI),
        None,
    )
    if not broken_diagnostics:
        return fail("a broken buffer reported no diagnostics")

    edits = by_id.get(2, {}).get("result")
    if not edits:
        return fail("formatting returned no edit for an unformatted buffer")
    if edits[0]["newText"] == UNFORMATTED:
        return fail("the formatting edit did not change the buffer")

    if by_id.get(3, {}).get("result", "missing") is not None:
        return fail("shutdown did not answer with null")

    print("lsp: smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
