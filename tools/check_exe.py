#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""End-to-end execution checks.

Each case is a directory under exe/cases/<name>/ holding main.al and
expect.toml:

    exit = 0
    stdout = "hi\n"
    stdout_contains = ["hi"]
    stderr_contains = ["boom"]

`exit` is required (integer value or "non-zero"). `stdout` must match
exactly when present; `*_contains` lists must all appear in the
respective stream. `walk_stderr_contains` is asserted where the host's
panic walk runs: the walk reads the x86-64 SysV and AArch64 frame
layout, and Windows x64's frame pointer is not that layout (ADR-0070).
An optional `args` list is passed to the alcy build itself, so a
single-file case can select what a package case declares in its own
files.

Cases are copied to a scratch directory, built to executables with
`alcy compile` for a single file or `alcy build` for a package, executed,
and asserted. Directories holding alcy.toml build as packages.
"""

import argparse
import concurrent.futures
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

from utils.env import run_environment
from utils.paths import project_root_dir


def parse_expect(path: Path):
    try:
        with open(path, "rb") as f:
            data = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        sys.exit(f"{path}: invalid TOML format: {e}")

    expected_exit = data.get("exit")
    if expected_exit is None:
        sys.exit(f"{path}: missing required 'exit' key")

    expected_stdout = data.get("stdout")
    stdout_contains = data.get("stdout_contains", [])
    stderr_contains = data.get("stderr_contains", [])
    walk_stderr_contains = data.get("walk_stderr_contains", [])

    # Extra arguments for the alcy build itself, so a single-file case
    # can select what a package case declares in its own files.
    args = data.get("args", [])
    if not isinstance(args, list) or not all(isinstance(arg, str) for arg in args):
        sys.exit(f"{path}: 'args' must be a list of strings")

    return (
        expected_exit,
        expected_stdout,
        stdout_contains,
        stderr_contains,
        walk_stderr_contains,
        args,
    )


# Sanitizers for a generated program. Address is the one that finds
# memory bugs in compiler output; undefined is here because a lowering
# mistake often shows up as an integer or shift operation that is merely
# wrong rather than fatal.
SANITIZERS = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]

# The system compiler, not alcy's, is what instruments a generated
# program. Without it the sanitized run cannot happen, and asking for one
# that cannot happen is a refusal rather than a downgrade: see main().
SYSTEM_CLANG = shutil.which("clang")


def mark_instrumentable(ir: Path) -> None:
    """Marks every definition in a textual module for instrumentation.

    A front end carries `-fsanitize=address` into each function it emits
    as the `sanitize_address` attribute, and the instrumentation pass
    reaches only functions that carry it. The alcy IR names no
    sanitizer, so a module handed straight to clang links the runtime
    while every load and store stays unchecked, and the leak scan at
    exit is the only half that still reports. Setting the attribute on
    each definition is what lets the address checks see the generated
    program; without the pass the attribute reads as inert metadata.
    """
    text = ir.read_text(encoding="utf-8")
    marked = re.sub(r"(?m)^(define .*) \{$", r"\1 sanitize_address {", text)
    ir.write_text(marked, encoding="utf-8")


def build_sanitized(alcy: Path, work: Path, is_package: bool, exe: Path, args):
    """Builds the case through textual IR so the sanitizers can reach it.

    alcy writes an object straight from an LLVM TargetMachine, with no
    external compiler in the path, so the code it generates cannot be
    instrumented after the fact. Textual IR can: the module is handed to
    clang, which both compiles and instruments it, runtime included.
    That is the reason --emit=llvm-ir exists, and this is what it is
    for. The marking above is what points the instrumentation at the
    functions clang could otherwise not see.
    """
    ir = work / "main.ll"
    command = "build" if is_package else "compile"
    argv = [
        str(alcy),
        command,
        "." if is_package else "main.al",
        *args,
        "--emit=llvm-ir",
        "-o",
        str(ir),
    ]
    proc = subprocess.run(
        argv,
        capture_output=True,
        text=True,
        encoding="utf-8",
        cwd=work,
        env=run_environment(),
    )
    if proc.returncode != 0 or not ir.is_file():
        return proc, "alcy --emit=llvm-ir did not produce a module"

    mark_instrumentable(ir)
    link = [SYSTEM_CLANG, "-x", "ir", str(ir), *SANITIZERS, "-o", str(exe)]
    proc = subprocess.run(
        link, capture_output=True, text=True, encoding="utf-8", cwd=work
    )
    if proc.returncode != 0:
        return proc, "clang could not compile the generated IR"
    return proc, ""


def is_freestanding(case_dir: Path) -> bool:
    """Whether the goal package links without a C runtime.

    The toolchain file says so (ADR-0052); a freestanding program owns
    its `_start`, so the sanitizer's crt and libraries cannot join it.
    """
    toolchain = case_dir / ".alcy" / "toolchain.toml"
    if not toolchain.is_file():
        return False
    with open(toolchain, "rb") as f:
        data = tomllib.load(f)
    return bool(data.get("freestanding", False))


def host_walks_frames() -> bool:
    """Whether the host's panic walk runs (ADR-0070).

    The walk reads the x86-64 SysV and AArch64 frame layout; Windows
    x64's frame pointer is an RSP-relative point in the fixed frame by
    design, and any other machine has no layout the walk knows.
    """
    if sys.platform == "win32":
        return False
    return platform.machine().lower() in {"x86_64", "aarch64", "arm64"}


def run_tool(args: list[str]) -> str:
    proc = subprocess.run(args, capture_output=True, text=True, errors="replace")
    return proc.stdout


def image_facts(exe: Path) -> str:
    """What a failed walk resolution needs read back from the image.

    A frame line is the frame chain and the two tables answering
    together, so a line the source cannot explain is one of those three
    answering differently than the compiler meant. The symbols, the
    program functions' disassembly, and every word in the constant data
    that names an address are printed, so the answer can be read back on
    the host that produced it.
    """
    nm = run_tool(["llvm-nm", "-n", str(exe)])
    names: list[str] = []
    symbols: dict[int, str] = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        name = parts[-1]
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        symbols[addr] = name
        if name.endswith((":g", ":f", ":alcy_main")) or name in (
            "alcy_panic",
            "_alcy_panic",
            "alcy_write_all",
            "_alcy_write_all",
        ):
            names.append(name)

    out = ["--- symbols ---", nm.rstrip()]
    if names:
        disassembly = run_tool(
            [
                "llvm-objdump",
                "-d",
                f"--disassemble-symbols={','.join(names)}",
                str(exe),
            ]
        )
        out += ["--- program ---", disassembly.rstrip()]

    # Every eight-byte word naming an address, wherever it is: the tables
    # are the ones that matter, and a word that names a function or a
    # name string is one of their entries. Words are read from the
    # constant sections, where the linker places both.
    floor = min(symbols) if symbols else 0
    lines: list[str] = []
    in_constant = False
    for line in run_tool(["llvm-objdump", "-s", str(exe)]).splitlines():
        header = re.match(r"Contents of section (\S+):", line)
        if header is not None:
            name = header.group(1)
            in_constant = "const" in name or ".data.rel.ro" in name
            continue
        if not in_constant:
            continue
        m = re.match(r"\s*([0-9a-f]{4,16})\s+((?:[0-9a-f]{2,8}\s*)+)", line)
        if m is None:
            continue
        base = int(m.group(1), 16)
        hexstr = m.group(2).split("  ")[0].replace(" ", "")
        if len(hexstr) % 2:
            hexstr = hexstr[:-1]
        blob = bytes.fromhex(hexstr)
        for offset in range(0, len(blob) - 7, 8):
            raw = int.from_bytes(blob[offset : offset + 8], "little")
            # A plain pointer, or a chained rebase fixup whose low 36
            # bits are the address it will carry once dyld is done.
            values = [raw]
            if raw >> 63 == 0:
                values.append(raw & ((1 << 36) - 1))
            for value in values:
                if floor <= value < floor + 0x10000000:
                    near = max((a for a in symbols if a <= value), default=None)
                    label = symbols.get(value, symbols.get(near, "?"))
                    lines.append(f"{base + offset:#010x} {value:#010x} {label}")
                    break
    out += ["--- words naming an address ---", "\n".join(lines)]
    return "\n".join(out) + "\n"


def run_case(alcy: Path, case_dir: Path, sanitize: bool = False):
    is_package = (case_dir / "alcy.toml").is_file()
    main_al = case_dir / "main.al"
    expect_path = case_dir / "expect.toml"

    if not is_package and not main_al.is_file():
        return False, "no main.al found"
    if not expect_path.is_file():
        return False, "missing expect.toml"

    if sanitize and is_freestanding(case_dir):
        # A freestanding program defines its own entry and links no C
        # runtime, so it runs uninstrumented rather than not at all.
        sanitize = False

    (
        expected_exit,
        expected_stdout,
        stdout_contains,
        stderr_contains,
        walk_stderr_contains,
        extra_args,
    ) = parse_expect(expect_path)
    if host_walks_frames():
        stderr_contains = [*stderr_contains, *walk_stderr_contains]
    problems = []
    missing_walk = False

    with tempfile.TemporaryDirectory(prefix="alcy_exe_case") as tmp:
        work = Path(tmp)
        exe_name = "main_exe.exe" if os.name == "nt" else "main_exe"

        if is_package:
            shutil.copytree(
                case_dir,
                work,
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("expect.toml"),
            )
            target = "."
        else:
            shutil.copy(main_al, work / "main.al")
            target = "main.al"

        command = "build" if is_package else "compile"
        if sanitize:
            proc, why = build_sanitized(
                alcy, work, is_package, work / exe_name, extra_args
            )
            if why:
                return False, f"{why}\n--- stderr ---\n{proc.stderr}"
        else:
            build_argv = [str(alcy), command, target, *extra_args, "-o", exe_name]
            proc = subprocess.run(
                build_argv,
                capture_output=True,
                text=True,
                encoding="utf-8",
                cwd=work,
                env=run_environment(),
            )
        if proc.returncode != 0:
            return False, (
                f"alcy {command} failed: exit={proc.returncode}\n"
                f"--- build stdout ---\n{proc.stdout}"
                f"--- build stderr ---\n{proc.stderr}"
            )

        exe_path = work / exe_name
        if not exe_path.is_file():
            return False, (
                f"alcy build succeeded but '{exe_name}' is missing\n"
                f"--- build stdout ---\n{proc.stdout}"
                f"--- build stderr ---\n{proc.stderr}"
            )

        try:
            proc = subprocess.run(
                [str(exe_path)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                cwd=work,
            )
        except OSError as e:
            return False, f"cannot execute '{exe_name}': {e}"

        if isinstance(expected_exit, str) and expected_exit.lower() in (
            "non-zero",
            "nonzero",
            "!0",
        ):
            if proc.returncode == 0:
                problems.append(f"exit: got {proc.returncode}, want non-zero")
        elif proc.returncode != expected_exit:
            problems.append(f"exit: got {proc.returncode}, want {expected_exit}")

        if expected_stdout is not None and proc.stdout != expected_stdout:
            problems.append(f"stdout: got {proc.stdout!r}, want {expected_stdout!r}")
        for needle in stdout_contains:
            if needle not in proc.stdout:
                problems.append(f"missing stdout: {needle!r}")
        for needle in stderr_contains:
            if needle not in proc.stderr:
                problems.append(f"missing stderr: {needle!r}")
                if needle in walk_stderr_contains:
                    missing_walk = True

        # A sanitizer writes its report to stderr and exits non-zero, which
        # the exit check may already have caught. This catches the case
        # where a case expects a non-zero exit, so a report would pass for
        # an ordinary failure.
        for marker in (
            "AddressSanitizer",
            "runtime error:",
            "LeakSanitizer",
            "SEGV on unknown address",
        ):
            if marker in proc.stderr:
                problems.append(f"sanitizer report: {marker}")

        # Read the image back while it still exists: a frame the walk
        # resolved is a fact about the binary that resolved it.
        facts = image_facts(exe_path) if missing_walk else ""

    if problems:
        detail = "; ".join(problems)
        detail += f"\n--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        if facts:
            detail += "\n" + facts
        return False, detail
    return True, ""


def main():
    # Program output is UTF-8 on every platform; report it as such
    # instead of the console locale, so a unicode mismatch prints
    # rather than crashing the runner.
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Run execution cases.")
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary",
    )
    parser.add_argument(
        "--cases",
        default="",
        help="Comma-separated case names to run (default: all)",
    )
    parser.add_argument(
        "--cases-root",
        default="exe/cases",
        help=(
            "Directory of case directories, relative to the project root. "
            "The samples use the same harness as the test cases, so that a "
            "program kept as an example stays compiled and run."
        ),
    )
    parser.add_argument(
        "--sanitize",
        action="store_true",
        help=(
            "Build each case through --emit=llvm-ir and compile it with "
            "AddressSanitizer and UndefinedBehaviorSanitizer, so a bug in "
            "the code alcy generates is reported rather than inherited."
        ),
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help=(
            "Cases to run concurrently (default: the runner's core count, "
            "capped at 8). A case is a pipeline of subprocesses -- the "
            "compiler, the system link, the run -- so concurrent cases wait "
            "on their children rather than on each other."
        ),
    )
    args = parser.parse_args()

    if args.sanitize and SYSTEM_CLANG is None:
        # A run without the sanitizer would pass while testing none of what
        # was asked for, so this refuses rather than degrades. A gate that
        # cannot discriminate is worse than no gate: it is green for a
        # reason nobody reading it can see.
        print(
            "error: --sanitize needs clang on PATH, and it is not there",
            file=sys.stderr,
        )
        return 1

    alcy = project_root_dir / "out" / args.build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    if not alcy.is_file():
        print(f"alcy binary not found in out/{args.build_subdir}/")
        return -1

    cases_root = project_root_dir / args.cases_root
    if not cases_root.is_dir():
        print(f"cases root not found: {args.cases_root}")
        return -1
    selected = (
        {name.strip() for name in args.cases.split(",") if name.strip()}
        if args.cases
        else None
    )
    # A freestanding program is Linux's syscall ABI and an `_start` the
    # kernel enters; another host's pipeline refuses the target, which is
    # the compiler working rather than the case failing.
    runs_freestanding = sys.platform.startswith("linux")
    # A case is a subprocess pipeline of its own -- the compiler, the
    # system link, the run -- so the pool waits on processes rather than
    # holding the interpreter, and the order the cases finish in is the
    # order the results are reported in, which is the sorted order they
    # were handed out in.
    jobs = args.jobs or min(os.cpu_count() or 2, 8)
    selected_dirs = []
    skipped = 0
    for case_dir in sorted(cases_root.iterdir()):
        if not case_dir.is_dir():
            continue
        if selected is not None and case_dir.name not in selected:
            continue
        if not runs_freestanding and is_freestanding(case_dir):
            skipped += 1
            continue
        selected_dirs.append(case_dir)

    def checked(case_dir: Path):
        try:
            return run_case(alcy, case_dir, args.sanitize)
        except Exception as e:  # noqa: BLE001 - a harness bug is a case failure
            return False, f"the harness raised: {e!r}"

    failures = 0
    ran = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for case_dir, (ok, detail) in zip(
            selected_dirs, pool.map(checked, selected_dirs), strict=True
        ):
            ran += 1
            print(f"{'PASS' if ok else 'FAIL'} {case_dir.name}")
            if not ok:
                failures += 1
                print(detail)
    sys.stdout.flush()
    if skipped:
        print(
            f"note: {skipped} freestanding case(s) skipped: "
            f"{sys.platform} is not a Linux syscall host"
        )
    print(f"exe: {ran - failures}/{ran} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
