#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Embeds the toolchain standard library sources as byte arrays.

Reads `lib/std/alcy.toml` and each member's manifest, then emits a
translation unit exposing the bytes of every staged `.al` file, the
suite's dependency edges, and the public names of each member. Byte
arrays stay portable across hosts without external tools or
per-architecture handling.
"""

import argparse
import re
import sys
import tomllib
from pathlib import Path


def emit_array(out, symbol, data: bytes):
    out.write(f"const u8 {symbol}[] = {{\n")
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        out.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    out.write("};\n")
    out.write(f"const u64 {symbol}_LEN = {len(data)}ULL;\n\n")


def main():
    parser = argparse.ArgumentParser(description="Embed std sources.")
    parser.add_argument("--std-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    std_dir = Path(args.std_dir)

    # The suite manifest names the members in order; each package
    # manifest names its staged modules and its dependencies on other
    # members. The manifests are the single source of truth: the
    # generated tables below carry them into the binary.
    with open(std_dir / "alcy.toml", "rb") as f:
        suite = tomllib.load(f)["suite"]
    if suite["owner"] != "alcy" or suite["name"] != "std":
        raise SystemExit("lib/std/alcy.toml is not the alcy/std suite")
    members = suite["packages"]
    if sorted(members) != sorted([q.name for q in std_dir.iterdir() if q.is_dir()]):
        raise SystemExit("suite members and lib/std directories disagree")
    pkg_modules = {}
    pkg_deps = {}
    pkg_specs = {}
    for name in members:
        with open(std_dir / name / "alcy.toml", "rb") as f:
            manifest = tomllib.load(f)
        if manifest["package"]["name"] != name:
            raise SystemExit(f"lib/std/{name}/alcy.toml names another package")
        pkg_modules[name] = manifest["modules"]["include"]
        # The [spec] table's declaring side, read the way the manifest
        # parser reads it: only `suite-only`, a list of spec names. The
        # compiler never parses a staged member's manifest, so the
        # generated table is where its seal must be checked.
        spec_table = manifest.get("spec", {})
        unknown = set(spec_table) - {"suite-only"}
        if unknown:
            raise SystemExit(
                f"lib/std/{name}/alcy.toml [spec] has unknown keys: {sorted(unknown)}"
            )
        seals = spec_table.get("suite-only", [])
        if not isinstance(seals, list) or any(
            not isinstance(seal, str) or not seal for seal in seals
        ):
            raise SystemExit(
                f"lib/std/{name}/alcy.toml [spec] suite-only must be a list of names"
            )
        if len(set(seals)) != len(seals):
            raise SystemExit(
                f"lib/std/{name}/alcy.toml [spec] suite-only repeats a name"
            )
        pkg_specs[name] = seals
        deps = []
        for spec in manifest.get("dependencies", {}):
            parts = spec.split("/")
            # Only suite-internal edges are embedded; anything else is
            # for a resolver the compiler does not have yet.
            if len(parts) != 3 or parts[0] != "alcy" or parts[1] != "std":
                raise SystemExit(
                    f"lib/std/{name}/alcy.toml depends outside the suite: {spec}"
                )
            if parts[2] == "*":
                raise SystemExit(
                    f"lib/std/{name}/alcy.toml globs the suite; name members"
                )
            deps.append(parts[2])
        pkg_deps[name] = deps
    # Every dependency edge must name a member, and the graph must be
    # acyclic; an embedded cycle would hang staging with no diagnostic.
    for name, deps in pkg_deps.items():
        for dep in deps:
            if dep not in members:
                raise SystemExit(
                    f"lib/std/{name}/alcy.toml depends on unknown member {dep}"
                )
    visiting = set()
    visited = set()

    def visit(node, stack):
        if node in visited:
            return
        if node in visiting:
            raise SystemExit(
                "dependency cycle in the suite: " + " -> ".join(stack + [node])
            )
        visiting.add(node)
        for dep in pkg_deps[node]:
            visit(dep, stack + [node])
        visiting.remove(node)
        visited.add(node)

    for name in members:
        visit(name, [])

    files = {}
    for name in members:
        for mod in pkg_modules[name]:
            symbol = f"STD_{name.upper()}_{mod.upper()}"
            files[symbol] = std_dir / name / (mod + ".al")

    with open(args.output, "w", encoding="utf-8") as out:
        out.write(
            "// Copyright 2026 The Alcy Project Authors\n"
            "// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception\n"
            "// Generated by tools/embed_std.py; do not edit.\n"
            "\n"
            '#include "pipeline/embedded_std.h"\n'
            "\n"
            '#include "fpag/base/numeric.h"\n'
            "\n"
            "namespace pipeline {\n"
            "\n"
        )
        entries = []
        for symbol, path in files.items():
            emit_array(out, symbol, path.read_bytes())
            entries.append((symbol, path))
        out.write("const StagedSource STAGED_SOURCES[] = {\n")
        for symbol, path in entries:
            rel = path.relative_to(std_dir).as_posix()
            out.write(f'  {{"{rel}", {symbol}, {symbol}_LEN}},\n')
        out.write("};\n\n")
        out.write(
            "const usize STAGED_SOURCE_COUNT = "
            "sizeof(STAGED_SOURCES) / sizeof(STAGED_SOURCES[0]);\n\n"
        )
        # The dependency edges each package manifest declares, as member
        # names. The selection layer expands suite globs against these
        # and rejects a selection whose closure is incomplete.
        for name in members:
            deps = pkg_deps[name]
            if deps:
                listed = ", ".join(f'"{d}"' for d in deps)
                out.write(
                    f"const char* const STD_DEPS_{name.upper()}[] = {{{listed}}};\n\n"
                )
        out.write("const StdPackageDeps STD_PACKAGE_DEPS[] = {\n")
        for name in members:
            deps = pkg_deps[name]
            table = f"STD_DEPS_{name.upper()}" if deps else "nullptr"
            out.write(f'  {{"{name}", {table}, {len(deps)}}},\n')
        out.write("};\n\n")
        out.write(
            "const usize STD_PACKAGE_COUNT = "
            "sizeof(STD_PACKAGE_DEPS) / sizeof(STD_PACKAGE_DEPS[0]);\n\n"
        )
        # The specs each member seals to the suite, from its [spec]
        # table. The compiler checks the staged member's declarations
        # against these rows (ADR-0053).
        for name in members:
            seals = pkg_specs[name]
            if seals:
                listed = ", ".join(f'"{seal}"' for seal in seals)
                out.write(
                    f"const std::string_view STD_SPECS_{name.upper()}[] = "
                    f"{{{listed}}};\n\n"
                )
        out.write("const StdPackageSeals STD_PACKAGE_SEALS[] = {\n")
        for name in members:
            seals = pkg_specs[name]
            table = f"STD_SPECS_{name.upper()}" if seals else "nullptr"
            out.write(f'  {{"{name}", {table}, {len(seals)}}},\n')
        out.write("};\n\n")
        out.write(
            "const usize STD_PACKAGE_SEAL_COUNT = "
            "sizeof(STD_PACKAGE_SEALS) / sizeof(STD_PACKAGE_SEALS[0]);\n\n"
        )
        # Every public name each package carries, for the missing
        # dependency hint: an unresolved name found here names the
        # package to add. Top-level `pub` items only; methods are
        # indented and never match the anchor.
        item_re = re.compile(
            r"^pub\s+(?:intrinsic\s+)?(?:fn|struct|enum)\s+([A-Za-z_][A-Za-z0-9_]*)"
        )
        use_re = re.compile(r"^pub\s+use\s+.*::([A-Za-z_][A-Za-z0-9_]*)\s*;")
        symbols = []
        seen = set()
        for name in members:
            for mod in pkg_modules[name]:
                text = (std_dir / name / (mod + ".al")).read_text(encoding="utf-8")
                for line in text.split("\n"):
                    m = item_re.match(line) or use_re.match(line)
                    if m is not None and (name, m.group(1)) not in seen:
                        seen.add((name, m.group(1)))
                        symbols.append((name, m.group(1)))
        # One table, not two: the analyzer's hint type is the only reader,
        # and a second copy of the same rows under another name would be a
        # table with no consumer and two things to keep in step.
        out.write("const analyzer::StdHint STD_HINTS[] = {\n")
        for package, item in symbols:
            out.write(f'  {{"{package}", "{item}"}},\n')
        out.write("};\n\n")
        out.write(
            "const usize STD_HINT_COUNT = sizeof(STD_HINTS) / sizeof(STD_HINTS[0]);\n\n"
        )
        out.write("}  // namespace pipeline\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
