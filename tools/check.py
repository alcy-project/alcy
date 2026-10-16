#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""The union of gates a contributor runs before pushing.

Every command here is one CI asks in its own job
(docs/adr/0040-gates-live-beside-the-builds.md); the flags skip a gate
whose tool a machine lacks, and a skipped gate refuses or says so rather
than passing in silence. `./tools/check.sh` execs this, so the entry
point the documentation names still works.
"""

import argparse
import os
import shutil
import subprocess
import sys

from utils.paths import project_root_dir


def python_runner() -> list[str]:
    """The prefix that runs the other tools, or nothing if none works."""
    if shutil.which("uv") is not None:
        return ["uv", "run"]
    if shutil.which("python3") is not None:
        return ["python3"]
    sys.exit("error: uv or python3 is required to run this script")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run every local gate, in the order CI asks them."
    )
    parser.add_argument(
        "--nix",
        action="store_true",
        help="run all checks in a nix develop environment; the wasm gates "
        "need the .#wasm shell, which carries Emscripten",
    )
    parser.add_argument(
        "--no-wasm",
        action="store_true",
        help="skip the WebAssembly gates: the emscripten build, the "
        "direct-backend acceptance, and the playground smoke (they need "
        "Emscripten and bun)",
    )
    parser.add_argument(
        "--no-release",
        action="store_true",
        help="skip the gates that run the release build: the tests, the "
        "e2e cases, and the exe programs",
    )
    parser.add_argument(
        "--no-sanitize",
        action="store_true",
        help="skip the sanitized build of the exe cases (needs clang, and "
        "roughly doubles their runtime)",
    )
    parser.add_argument(
        "--no-coverage",
        action="store_true",
        help="skip the coverage ratchet (needs llvm-cov, and rebuilds the "
        "tests and the compiler with instrumentation)",
    )
    parser.add_argument(
        "--no-grammar",
        action="store_true",
        help="skip the tree-sitter checks (needs the tree-sitter CLI)",
    )
    args = parser.parse_args()

    # The absolute path survives the chdir below, which is what the nix
    # re-exec needs to find this script again from inside the shell.
    script = os.path.abspath(__file__)
    os.chdir(project_root_dir)
    py = python_runner()

    def run(*command: str) -> None:
        """One gate: the first failure is where the run stops."""
        if subprocess.run(list(command)).returncode != 0:
            sys.exit(f"error: gate failed: {' '.join(command)}")

    def tool(script: str, *flags: str) -> None:
        run(*py, f"./tools/{script}", *flags)

    def refuse(name: str, hint: str) -> None:
        sys.exit(f"error: {name} not found; {hint}")

    if args.nix and not os.environ.get("IN_NIX_SHELL"):
        # Enter the nix develop shell when asked, so the tool checks
        # below see the shell's tools rather than the host's: bun lives
        # in the default shell and emcc in the .#wasm one. Asking for the
        # shell and not having nix is refused rather than run outside the
        # shell it asked for.
        if shutil.which("nix") is None:
            sys.exit("error: nix not found; install nix first (or drop --nix)")
        os.execvp(
            "nix",
            ["nix", "develop", "-c", "uv", "run", "python", script, *sys.argv[1:]],
        )

    # The wasm gates are on by default, so a missing tool is worth saying
    # now rather than after every other gate has run.
    if not args.no_wasm:
        if shutil.which("emcc") is None:
            refuse(
                "emcc",
                "install Emscripten, or run from the .#wasm nix shell, "
                "or pass --no-wasm",
            )
        if shutil.which("bun") is None:
            refuse("bun", "install bun first (or pass --no-wasm)")

    if shutil.which("typos") is None:
        # The other optional tools here either refuse or say so; `typos`
        # was the one that skipped in silence, which is the one way a
        # skipped check can be mistaken for a check that passed.
        print(
            "note: typos not on PATH, so the spell check did not run",
            file=sys.stderr,
        )
    else:
        run("typos")

    if args.nix:
        release_subdir = "build_release_nix"
        debug_subdir = "build_nix"
        wasm_subdir = "build_wasm_nix"
    else:
        release_subdir = "build_release"
        debug_subdir = "build"
        wasm_subdir = "build_wasm"

    tool("build.py", "--target=all", "--mode=debug", f"--build-subdir={debug_subdir}")
    tool("run.py", "--target=tests", "--mode=debug", f"--build-subdir={debug_subdir}")
    tool(
        "build.py", "--target=all", "--mode=release", f"--build-subdir={release_subdir}"
    )

    if not args.no_release:
        # A bug only the release build has -- a DCHECK whose side effect
        # the release build drops, an optimization the debug build never
        # runs -- is invisible to every gate below that reads the debug
        # build, so the release binary answers the same three questions
        # here: the unit tests, the e2e cases, and the exe programs.
        tool(
            "run.py",
            "--target=tests",
            "--mode=release",
            f"--build-subdir={release_subdir}",
        )
        tool("check_e2e.py", f"--build-subdir={release_subdir}")
        tool("check_exe.py", f"--build-subdir={release_subdir}")

    tool("check_e2e.py", f"--build-subdir={debug_subdir}")
    tool("check_borrow_rules.py", f"--build-subdir={debug_subdir}")

    # The formatter's own gate, over every source rather than a sample:
    # the tree is what it would write, formatting formatted output
    # changes nothing, and the compiler's verdict about a file survives
    # its formatting.
    tool("check_fmt.py", f"--build-subdir={debug_subdir}")

    # The specification is what a program is written against, and the
    # compiler is how this one happens to implement it. That boundary is
    # prose in `docs/spec/overview.md` and a check here, since a
    # reference that creeps in is the kind of thing nobody notices until
    # the thing it names moves.
    tool("check_spec.py")

    # The checks below read the tree rather than build it. Each names
    # the tool it could not find: a check that does not run must not
    # read as one that passed.
    if shutil.which("taplo") is None:
        print(
            "note: taplo not on PATH, so the TOML check did not run",
            file=sys.stderr,
        )
    else:
        run("taplo", "fmt", "--check")

    if py[0] == "uv":
        run(*py, "ast-grep", "test")
        run(*py, "ast-grep", "scan", "--error", "compiler")
        run(*py, "rumdl", "check", ".")
        run(*py, "ruff", "check", "tools")
        run(*py, "ruff", "format", "--check", "tools")
    else:
        print(
            "note: uv not found, so the rule, Markdown and Python checks did not run",
            file=sys.stderr,
        )

    if not args.no_grammar:
        # The tree-sitter grammar is a second reading of the language,
        # so it is checked against the first: its own corpus here, and
        # every .al file in the repository against the binary the checks
        # above just built. It needs no toolchain beyond the CLI, which
        # is why the grammar half is not behind the build.
        if shutil.which("tree-sitter") is None:
            refuse("tree-sitter", "pass --no-grammar to skip")
        tool("check_treesitter.py", "--grammar")
        tool(
            "check_treesitter.py",
            "--differential",
            f"--build-subdir={debug_subdir}",
        )

    # The samples run through the harness the cases above use, so a
    # program kept as an example is also one that still compiles and
    # still prints what it says it prints. Here rather than with the
    # cases because these are read as much as run, and a sample that is
    # wrong is a documentation fault.
    tool(
        "check_exe.py",
        f"--build-subdir={debug_subdir}",
        "--cases-root=samples",
    )

    # The benchmark harness's own smoke: every case present, every
    # sample counted, a figure above zero. It judges shape and not
    # timing, so it runs anywhere, and CI asks it as its own job.
    tool("run_benchmarks.py", "smoke", f"--build-subdir={debug_subdir}")

    if not args.no_sanitize:
        # The same cases again through --emit=llvm-ir, so the code alcy
        # generates is compiled by an external toolchain that can
        # instrument it. Slow, and refused rather than skipped where
        # clang is absent: naming the flag is the difference between a
        # gate somebody can work with and one they have to read the
        # source of to get past.
        if shutil.which("clang") is None:
            refuse("clang", "pass --no-sanitize to skip")
        tool("check_exe.py", f"--build-subdir={debug_subdir}", "--sanitize")

    tool("format.py", "--dry-run")
    # Lint reuses the debug build's compile database: a dedicated GN
    # tree would install its own copy of LLVM for the same bytes.
    tool("lint.py", f"--build-subdir={debug_subdir}")

    if not args.no_coverage:
        # The coverage ratchet rebuilds instrumented binaries, so it
        # runs after the format and lint gates and reuses its own output
        # directory. Skip with --no-coverage when llvm-cov is absent.
        if shutil.which("llvm-cov") is None:
            refuse("llvm-cov", "pass --no-coverage to skip")
        tool("check_coverage.py")

    tool("verify_static_linkage.py", f"--build-dir=out/{release_subdir}")
    tool("verify_static_linkage.py", f"--build-dir=out/{debug_subdir}")

    if not args.no_wasm:
        tool(
            "run.py",
            "--target=tests",
            "--mode=debug",
            f"--build-subdir={wasm_subdir}",
            "--target-os=emscripten",
        )

        # The direct wasm backend's acceptance: the host compiler emits
        # the module and bun runs it under WASI. That is the half of the
        # wasm story the emscripten build above cannot ask, and CI runs
        # it in every matrix entry, so the local gate asks it too.
        tool("check_wasm.py", f"--build-subdir={debug_subdir}")

        # The playground carries no LLVM and no tests, so it is its own
        # configure; the smoke is what says the artifact a site loads
        # runs.
        playground_subdir = f"{wasm_subdir}_playground"
        tool(
            "build.py",
            "--target=playground",
            "--mode=debug",
            f"--build-subdir={playground_subdir}",
            "--target-os=emscripten",
            "--gn-arg=alcy_backends=[]",
        )
        tool("check_playground.py", f"--build-subdir={playground_subdir}")

    print("check ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
