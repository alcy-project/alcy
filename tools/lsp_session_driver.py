#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# One editor session against the real alcy lsp binary, timed from inside.
#
# The driver speaks the protocol the client would: initialize, open the
# document, and then run the phase the case asks for, measuring from the
# moment the request is written to the moment the answer arrives. The
# process spawn and the session setup before the phase are not in the
# number; the runner's repetitions pool the per-session figures.
#
#   lsp_session_driver.py --alcy OUT/opt/alcy --root DIR --phase open
#
# Prints one JSON object: {"ns": <measured nanoseconds>}.
#
# The edits the edit phase sends are the open text with one statement
# appended: valid, so the reading stays clean, and deterministic, so
# every repetition measures the same work.

import argparse
import json
import queue
import subprocess
import sys
import threading
import time


def frame(obj: dict) -> bytes:
    body = json.dumps(obj).encode()
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


class Session:
    def __init__(self, alcy: str, root: str):
        self.proc = subprocess.Popen(
            [alcy, "lsp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self.messages: queue.Queue = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "processId": None,
                    "rootUri": "file://" + root,
                    "capabilities": {},
                },
            }
        )
        self.wait_response(1, 30.0)
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

    def send(self, obj: dict) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(frame(obj))
        self.proc.stdin.flush()

    def _read(self) -> None:
        out = self.proc.stdout
        while True:
            headers = {}
            while True:
                line = out.readline()
                if not line:
                    self.messages.put(None)
                    return
                if line in (b"\r\n", b"\n"):
                    break
                key, _, value = line.decode().partition(":")
                headers[key.strip().lower()] = value.strip()
            size = int(headers.get("content-length", "0"))
            payload = out.read(size)
            if size and len(payload) == size:
                self.messages.put(json.loads(payload))

    def wait_response(self, request_id: int, timeout: float):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no response for id {request_id}")
            message = self.messages.get(timeout=remaining)
            if message is None:
                raise TimeoutError("the server closed its output")
            if message.get("id") == request_id and (
                "result" in message or "error" in message
            ):
                return message

    def wait_notification(self, method: str, uri: str | None, timeout: float):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no notification {method}")
            message = self.messages.get(timeout=remaining)
            if message is None:
                raise TimeoutError("the server closed its output")
            if message.get("method") != method:
                continue
            if uri is None or message.get("params", {}).get("uri") == uri:
                return message

    def open(self, uri: str, text: str) -> float:
        started = time.perf_counter_ns()
        self.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "alcy",
                        "version": 1,
                        "text": text,
                    }
                },
            }
        )
        self.wait_notification("textDocument/publishDiagnostics", uri, 60.0)
        return time.perf_counter_ns() - started

    def edit(self, uri: str, text: str) -> float:
        started = time.perf_counter_ns()
        self.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": text}],
                },
            }
        )
        self.wait_notification("textDocument/publishDiagnostics", uri, 60.0)
        return time.perf_counter_ns() - started

    def hover(self, uri: str, line: int, character: int) -> float:
        started = time.perf_counter_ns()
        self.send(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": line, "character": character},
                },
            }
        )
        self.wait_response(2, 60.0)
        return time.perf_counter_ns() - started

    def close(self) -> int:
        self.send({"jsonrpc": "2.0", "id": 99, "method": "shutdown"})
        self.wait_response(99, 30.0)
        self.send({"jsonrpc": "2.0", "method": "exit"})
        return self.proc.wait(timeout=30.0)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="One editor session against alcy lsp, timed from inside."
    )
    parser.add_argument("--alcy", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--file", default="main.al")
    parser.add_argument("--phase", required=True, choices=["open", "edit", "hover"])
    args = parser.parse_args()

    from pathlib import Path

    path = Path(args.root) / args.file
    text = path.read_text(encoding="utf-8")
    # The edit is the open text with one statement appended: valid, so
    # the reading stays clean, and identical across repetitions.
    edited = (
        text.replace("  ret 0\n", "  total += 0\n  ret 0\n", 1)
        if "  ret 0\n" in text
        else text + "\nfn lsp_edit_probe() {}\n"
    )
    uri = "file://" + str(path)

    session = Session(args.alcy, args.root)
    try:
        if args.phase == "open":
            measured = session.open(uri, text)
        else:
            # The edit and hover phases measure a request on a warm
            # reading: the open above is setup, not the number.
            session.open(uri, text)
            if args.phase == "edit":
                measured = session.edit(uri, edited)
            else:
                measured = session.hover(uri, 2, 12)
    finally:
        code = session.close()
    if code != 0:
        print(f"the server exited {code}", file=sys.stderr)
        return 1
    print(json.dumps({"ns": measured}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
