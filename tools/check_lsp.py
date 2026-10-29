#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Smoke test for `alcy lsp`.

The language server is a process an editor talks to over a pipe, so the
only honest check runs the built binary through a session: the framing,
the initialize handshake, the diagnostics a didOpen publishes, the edit
a formatting request returns, the outline a documentSymbol request
returns, the folds a foldingRange request returns, the definition and
references a navigation request returns, the completion items and the
signature help a call answers with, and the memory layout an hover
carries. The script drives one session, parses the framed responses,
and asserts what each request answered.

The session opens an unformatted but valid struct, a broken buffer, a
multi-line buffer with a comment run, and a buffer with a call, so one
run covers the empty-diagnostics case, the reported case, the
formatter's edit, the outline, both kinds of fold, navigation,
completion and signature help, and the struct layout both the
alcy/structLayout request and hover return.
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
FOLD_URI = "file:///fold.al"
NAV_URI = "file:///nav.al"
TYPE_URI = "file:///type.al"
SPEC_URI = "file:///spec.al"

# Lint-clean but not canonical: the diagnostics answer is empty while
# the formatter still has a rewrite to return, so one buffer drives both
# assertions.
UNFORMATTED = "struct  Point{x:i32}"
BROKEN = "fn main( -> i32 {\n}\n"
# A comment run and two brace pairs on different lines, so one
# foldingRange answer exercises both kinds of fold. The loop is the
# document whose while-true finding carries the smoke's quick fix.
FOLD = (
    "// one\n// two\nfn main() -> i32 {\n  while true {\n    ret 0\n  }\n  ret 1\n}\n"
)
# A named function called once, so definition and references both point
# at the declaration and the call.
NAV = "fn id(x: i32) -> i32 {\n  ret x\n}\nfn main() -> i32 {\n  ret id(1)\n}\n"
# A value of struct type, so a type definition request has a value whose
# checked type names a declaration.
TYPE = "struct Point {\n  x: i32,\n}\n\nfn take(p: Point) -> i32 {\n  ret p.x\n}\n"
# A spec and its one implementation, so an implementation request can
# find the method that supplies the spec's body.
SPEC = (
    "struct Tag {\n  n: i32,\n}\n"
    "spec Shown {\n  fn show(self: &Self) -> i32;\n}\n"
    "impl Shown for Tag {\n  fn show(self: &Self) -> i32 {\n    ret self.n\n  }\n}\n"
    "fn call_it(t: &Tag) -> i32 {\n  ret t.show()\n}\n"
)


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
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": FOLD_URI,
                    "languageId": "alcy",
                    "version": 1,
                    "text": FOLD,
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": NAV_URI,
                    "languageId": "alcy",
                    "version": 1,
                    "text": NAV,
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": TYPE_URI,
                    "languageId": "alcy",
                    "version": 1,
                    "text": TYPE,
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": SPEC_URI,
                    "languageId": "alcy",
                    "version": 1,
                    "text": SPEC,
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
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "alcy/structLayout",
            "params": {
                "textDocument": {"uri": MAIN_URI},
                "position": {"line": 0, "character": 9},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "textDocument/hover",
            "params": {
                "textDocument": {"uri": MAIN_URI},
                "position": {"line": 0, "character": 9},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": MAIN_URI}},
        },
        {
            "jsonrpc": "2.0",
            "id": 6,
            "method": "textDocument/foldingRange",
            "params": {"textDocument": {"uri": FOLD_URI}},
        },
        {
            "jsonrpc": "2.0",
            "id": 7,
            "method": "textDocument/definition",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 4, "character": 7},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 8,
            "method": "textDocument/references",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 0, "character": 4},
                "context": {"includeDeclaration": False},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 9,
            "method": "textDocument/signatureHelp",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 4, "character": 9},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 10,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 4, "character": 7},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 11,
            "method": "textDocument/inlayHint",
            "params": {"textDocument": {"uri": MAIN_URI}},
        },
        {
            "jsonrpc": "2.0",
            "id": 12,
            "method": "textDocument/codeAction",
            "params": {
                "textDocument": {"uri": FOLD_URI},
                "range": {
                    "start": {"line": 3, "character": 2},
                    "end": {"line": 3, "character": 14},
                },
                "context": {"diagnostics": []},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 13,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": MAIN_URI}},
        },
        {
            "jsonrpc": "2.0",
            "id": 14,
            "method": "textDocument/documentHighlight",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 4, "character": 7},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 15,
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": BROKEN_URI}},
        },
        {
            "jsonrpc": "2.0",
            "id": 16,
            "method": "textDocument/selectionRange",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "positions": [{"line": 4, "character": 7}],
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 17,
            "method": "textDocument/prepareRename",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 4, "character": 7},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 18,
            "method": "textDocument/rename",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 4, "character": 7},
                "newName": "ident",
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 19,
            "method": "textDocument/typeDefinition",
            "params": {
                "textDocument": {"uri": TYPE_URI},
                "position": {"line": 5, "character": 6},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 20,
            "method": "textDocument/implementation",
            "params": {
                "textDocument": {"uri": SPEC_URI},
                "position": {"line": 4, "character": 6},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 21,
            "method": "workspace/symbol",
            "params": {"query": "Point"},
        },
        {
            "jsonrpc": "2.0",
            "id": 22,
            "method": "textDocument/prepareCallHierarchy",
            "params": {
                "textDocument": {"uri": NAV_URI},
                "position": {"line": 4, "character": 7},
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 23,
            "method": "callHierarchy/incomingCalls",
            "params": {
                "item": {
                    "name": "id",
                    "kind": 12,
                    "uri": NAV_URI,
                    "range": {
                        "start": {"line": 0, "character": 0},
                        "end": {"line": 2, "character": 1},
                    },
                    "selectionRange": {
                        "start": {"line": 0, "character": 3},
                        "end": {"line": 0, "character": 5},
                    },
                    "data": {
                        "nameOffset": 3,
                        "nameLength": 2,
                        "rangeOffset": 0,
                        "rangeLength": 32,
                    },
                }
            },
        },
        {
            "jsonrpc": "2.0",
            "id": 24,
            "method": "textDocument/codeLens",
            "params": {"textDocument": {"uri": NAV_URI}},
        },
        {
            "jsonrpc": "2.0",
            "id": 25,
            "method": "textDocument/inlayHint",
            "params": {"textDocument": {"uri": NAV_URI}},
        },
        {"jsonrpc": "2.0", "id": 26, "method": "shutdown", "params": None},
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
    if capabilities.get("documentSymbolProvider") is not True:
        return fail("initialize did not advertise documentSymbolProvider")
    if capabilities.get("foldingRangeProvider") is not True:
        return fail("initialize did not advertise foldingRangeProvider")
    if capabilities.get("definitionProvider") is not True:
        return fail("initialize did not advertise definitionProvider")
    if capabilities.get("referencesProvider") is not True:
        return fail("initialize did not advertise referencesProvider")
    if not isinstance(capabilities.get("completionProvider"), dict):
        return fail("initialize did not advertise completionProvider")
    if not isinstance(capabilities.get("signatureHelpProvider"), dict):
        return fail("initialize did not advertise signatureHelpProvider")
    if capabilities.get("inlayHintProvider") is not True:
        return fail("initialize did not advertise inlayHintProvider")
    if capabilities.get("codeActionProvider") is not True:
        return fail("initialize did not advertise codeActionProvider")
    if not isinstance(capabilities.get("semanticTokensProvider"), dict):
        return fail("initialize did not advertise semanticTokensProvider")
    if capabilities.get("documentHighlightProvider") is not True:
        return fail("initialize did not advertise documentHighlightProvider")
    if capabilities.get("selectionRangeProvider") is not True:
        return fail("initialize did not advertise selectionRangeProvider")
    if capabilities.get("typeDefinitionProvider") is not True:
        return fail("initialize did not advertise typeDefinitionProvider")
    if capabilities.get("implementationProvider") is not True:
        return fail("initialize did not advertise implementationProvider")
    if capabilities.get("workspaceSymbolProvider") is not True:
        return fail("initialize did not advertise workspaceSymbolProvider")
    if capabilities.get("callHierarchyProvider") is not True:
        return fail("initialize did not advertise callHierarchyProvider")
    if not isinstance(capabilities.get("codeLensProvider"), dict):
        return fail("initialize did not advertise codeLensProvider")
    rename = capabilities.get("renameProvider")
    if not isinstance(rename, dict) or rename.get("prepareProvider") is not True:
        return fail("initialize did not advertise renameProvider")
    if not isinstance(capabilities.get("diagnosticProvider"), dict):
        return fail("initialize did not advertise diagnosticProvider")
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

    layout = by_id.get(3, {}).get("result")
    if not isinstance(layout, dict) or layout.get("name") != "Point":
        return fail(f"structLayout did not name the struct: {layout!r}")
    if layout.get("size") != 4 or layout.get("align") != 4:
        return fail(f"structLayout gave the wrong shape: {layout!r}")
    if layout.get("fields") != [
        {"name": "x", "type": "i32", "offset": 0, "size": 4, "align": 4}
    ]:
        return fail(f"structLayout gave the wrong fields: {layout!r}")

    hover = by_id.get(4, {}).get("result")
    contents = hover.get("contents", {}) if isinstance(hover, dict) else {}
    if contents.get("kind") != "markdown" or "size **4**" not in contents.get(
        "value", ""
    ):
        return fail(f"hover did not carry the layout: {hover!r}")

    symbols = by_id.get(5, {}).get("result")
    point_symbol = None
    if isinstance(symbols, list):
        point_symbol = next((s for s in symbols if s.get("name") == "Point"), None)
    if point_symbol is None or point_symbol.get("kind") != 23:
        return fail(f"documentSymbol missed the struct: {symbols!r}")
    if not any(
        child.get("name") == "x" for child in (point_symbol.get("children") or [])
    ):
        return fail(f"documentSymbol missed the field: {point_symbol!r}")

    folds = by_id.get(6, {}).get("result")

    def has_fold(start: int, end: int) -> bool:
        return any(
            fold.get("startLine") == start and fold.get("endLine") == end
            for fold in (folds or [])
        )

    if not has_fold(2, 7):
        return fail(f"foldingRange missed the function body: {folds!r}")
    comment_fold = next(
        (
            fold
            for fold in (folds or [])
            if fold.get("startLine") == 0 and fold.get("endLine") == 1
        ),
        None,
    )
    if comment_fold is None or comment_fold.get("kind") != "comment":
        return fail(f"foldingRange missed the comment run: {folds!r}")

    definition = by_id.get(7, {}).get("result")
    if not isinstance(definition, dict):
        return fail(f"definition answered nothing: {definition!r}")
    if definition.get("range", {}).get("start") != {"line": 0, "character": 3}:
        return fail(f"definition pointed elsewhere: {definition!r}")

    references = by_id.get(8, {}).get("result")
    if not isinstance(references, list) or len(references) != 1:
        return fail(f"references did not find the call: {references!r}")
    if references[0].get("range", {}).get("start") != {"line": 4, "character": 6}:
        return fail(f"references pointed elsewhere: {references!r}")

    signature = by_id.get(9, {}).get("result")
    if not isinstance(signature, dict):
        return fail(f"signatureHelp answered nothing: {signature!r}")
    signature_label = (signature.get("signatures") or [{}])[0].get("label")
    if signature_label != "fn id(i32) -> i32":
        return fail(f"signatureHelp gave the wrong label: {signature_label!r}")

    completion = by_id.get(10, {}).get("result")
    items = completion.get("items") if isinstance(completion, dict) else None
    labels = {item.get("label") for item in (items or [])}
    if "id" not in labels:
        return fail(f"completion missed the function: {completion!r}")

    hints = by_id.get(11, {}).get("result")
    hint_labels = [hint.get("label") for hint in (hints or [])]
    if not any(
        isinstance(label, str) and label.startswith("size ") for label in hint_labels
    ):
        return fail(f"inlayHint missed the layout: {hints!r}")

    actions = by_id.get(12, {}).get("result")
    if not isinstance(actions, list) or not actions:
        return fail(f"codeAction offered nothing: {actions!r}")
    if actions[0].get("kind") != "quickfix":
        return fail(f"codeAction was not a quick fix: {actions!r}")
    if not isinstance(actions[0].get("edit"), dict):
        return fail(f"codeAction carried no edit: {actions!r}")

    tokens = by_id.get(13, {}).get("result")
    token_data = tokens.get("data") if isinstance(tokens, dict) else None
    if not isinstance(token_data, list) or not token_data:
        return fail(f"semanticTokens returned no data: {tokens!r}")
    if len(token_data) % 5 != 0:
        return fail(f"semanticTokens data is not five per token: {tokens!r}")

    highlights = by_id.get(14, {}).get("result")
    if not isinstance(highlights, list) or len(highlights) < 2:
        return fail(f"documentHighlight missed the occurrences: {highlights!r}")

    pull = by_id.get(15, {}).get("result")
    if not isinstance(pull, dict) or pull.get("kind") != "full":
        return fail(f"diagnostic was not a full report: {pull!r}")
    if not pull.get("items"):
        return fail(f"diagnostic reported a broken buffer clean: {pull!r}")

    selections = by_id.get(16, {}).get("result")
    if not isinstance(selections, list) or len(selections) != 1:
        return fail(f"selectionRange answered the wrong count: {selections!r}")
    depth = 0
    node = selections[0]
    while isinstance(node, dict) and "range" in node:
        depth += 1
        node = node.get("parent")
    if depth < 3:
        return fail(f"selectionRange did not grow past the word: {selections!r}")

    prepare = by_id.get(17, {}).get("result")
    if not isinstance(prepare, dict) or prepare.get("placeholder") != "id":
        return fail(f"prepareRename did not name id: {prepare!r}")

    renamed = by_id.get(18, {}).get("result")
    rename_edits = (
        renamed.get("changes", {}).get(NAV_URI) if isinstance(renamed, dict) else None
    )
    if not isinstance(rename_edits, list) or len(rename_edits) < 2:
        return fail(f"rename did not edit both occurrences: {renamed!r}")

    typed = by_id.get(19, {}).get("result")
    typed_start = (
        typed.get("range", {}).get("start") if isinstance(typed, dict) else None
    )
    if not isinstance(typed_start, dict) or typed_start.get("line") != 0:
        return fail(f"typeDefinition did not name Point: {typed!r}")

    implementations = by_id.get(20, {}).get("result")
    impl_start = (
        (implementations or [{}])[0].get("range", {}).get("start")
        if isinstance(implementations, list)
        else None
    )
    if not isinstance(impl_start, dict) or impl_start.get("line") != 7:
        return fail(f"implementation missed the impl method: {implementations!r}")

    workspace = by_id.get(21, {}).get("result")
    if not isinstance(workspace, list) or not any(
        isinstance(symbol, dict) and symbol.get("name") == "Point"
        for symbol in workspace
    ):
        return fail(f"workspace/symbol missed Point: {workspace!r}")

    hierarchy = by_id.get(22, {}).get("result")
    if (
        not isinstance(hierarchy, list)
        or not hierarchy
        or hierarchy[0].get("name") != "id"
    ):
        return fail(f"prepareCallHierarchy missed id: {hierarchy!r}")

    incoming = by_id.get(23, {}).get("result")
    if not isinstance(incoming, list) or len(incoming) != 1:
        return fail(f"incomingCalls missed the caller: {incoming!r}")
    if incoming[0].get("from", {}).get("name") != "main":
        return fail(f"incomingCalls named the wrong caller: {incoming!r}")

    lenses = by_id.get(24, {}).get("result")
    if not isinstance(lenses, list) or not any(
        isinstance(lens, dict) and lens.get("command", {}).get("title") == "1 reference"
        for lens in lenses
    ):
        return fail(f"codeLens missed the reference count: {lenses!r}")

    parameter_hints = by_id.get(25, {}).get("result")
    if not any(
        isinstance(hint, dict) and hint.get("label") == "x:"
        for hint in (parameter_hints or [])
    ):
        return fail(f"inlayHint missed the parameter name: {parameter_hints!r}")

    if by_id.get(26, {}).get("result", "missing") is not None:
        return fail("shutdown did not answer with null")

    print("lsp: smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
