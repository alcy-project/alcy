#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""The `[program]` step: build and execute the program a case produced.

Native, sanitized, and direct-wasm executions share the case's
expectation; only the way the program is built and started changes.
"""

import os
import platform
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

from cases.config import Case
from utils.env import run_environment
from utils.paths import project_root_dir

SANITIZERS = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]

# The system compiler, not Alcy's, is what instruments a generated
# program. Without it the sanitized run cannot happen, and asking for one
# that cannot happen is a refusal rather than a downgrade: see main().
SYSTEM_CLANG = shutil.which("clang")

WASM_TARGET = "wasm32-unknown-emscripten"
WASM_BACKEND = "direct-wasm"
WASM_MODULE = "main.wasm"

# A riscv64 freestanding program is emitted as an object, linked with the
# system clang/lld, and run under qemu-user. The backend is young, so a
# case that names a skip in case.toml is reported as a note, not a failure.
RISCV_TARGET = "riscv64-unknown-linux-gnu"
RISCV_BACKEND = "direct-riscv64"
QEMU = shutil.which("qemu-riscv64")

# Bun runs the module against WASI. Without it the differential cannot
# happen, which is a refusal for the same reason as a missing clang.
WASM_INTERPRETER = shutil.which("bun")
WASM_SHIM = project_root_dir / "tools" / "wasm_run.mjs"


def mark_instrumentable(ir: Path) -> None:
    """Marks every definition in a textual module for instrumentation.

    A front end carries `-fsanitize=address` into each function it emits
    as the `sanitize_address` attribute, and the instrumentation pass
    reaches only functions that carry it. The Alcy IR names no
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

    Alcy writes an object straight from an LLVM TargetMachine, with no
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
        errors="replace",
        cwd=work,
        env=run_environment(),
    )
    if proc.returncode != 0 or not ir.is_file():
        return proc, "alcy --emit=llvm-ir did not produce a module"

    mark_instrumentable(ir)
    link = [SYSTEM_CLANG, "-x", "ir", str(ir), *SANITIZERS, "-o", str(exe)]
    proc = subprocess.run(
        link,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        cwd=work,
    )
    if proc.returncode != 0:
        return proc, "clang could not compile the generated IR"
    return proc, ""


def is_freestanding(case_dir: Path) -> bool:
    """Whether the goal package links without a C runtime.

    The toolchain file says so ; a freestanding program owns
    its `_start`, so the sanitizer's crt and libraries cannot join it.
    """
    toolchain = case_dir / ".alcy" / "toolchain.toml"
    if not toolchain.is_file():
        return False
    with open(toolchain, "rb") as f:
        data = tomllib.load(f)
    return bool(data.get("freestanding", False))


def host_walks_frames() -> bool:
    """Whether the host's panic walk runs.

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


# The first four bytes name an image's container, so the judgement does
# not depend on text a tool prints about the path it read.
ELF_MAGIC = b"\x7fELF"
MACHO_MAGICS = frozenset(
    (
        b"\xfe\xed\xfa\xce",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe",
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf",
        b"\xbf\xba\xfe\xca",
    )
)


def image_format(exe: Path) -> str:
    """The container the image is written in, from its magic bytes."""
    with open(exe, "rb") as f:
        magic = f.read(4)
    if magic == ELF_MAGIC:
        return "ELF"
    if magic in MACHO_MAGICS:
        return "Mach-O"
    if magic[:2] == b"MZ":
        with open(exe, "rb") as f:
            f.seek(0x3C)
            pe_offset = int.from_bytes(f.read(4), "little")
            f.seek(pe_offset)
            if f.read(4) == b"PE\0\0":
                return "PE"
    return "unknown"


def foreign_imports(exe: Path, freestanding: bool) -> tuple[str, list[str]]:
    """What a produced image imports beyond the sanctioned ones.

    A freestanding Linux build links no C runtime at all, so an import
    table of anything but empty means the system C library crept in.
    A hosted Linux build links the system C runtime by default, so
    `libc.so.6` is sanctioned there as well. Windows links `kernel32`
    in both modes: an import table with anything else means the CRT or
    a C++ runtime crept in. A Mach-O image keeps libSystem, which is
    the one sanctioned route there, so it is not judged.
    """
    fmt = image_format(exe)
    if fmt == "ELF":
        allowed = set() if freestanding else {"libc.so.6", "libc.so"}
    elif fmt == "PE":
        allowed = {"kernel32.dll"}
    else:
        return fmt, []
    out = run_tool(["llvm-readobj", "--needed-libs", str(exe)])
    libs = []
    in_block = False
    for raw in out.splitlines():
        line = raw.strip()
        if line.startswith("NeededLibraries"):
            in_block = True
            continue
        if in_block:
            if line == "]" or not line:
                continue
            libs.append(line)
    return fmt, [lib for lib in libs if lib.lower() not in allowed]


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


def compare_streams(
    expected_exit,
    expected_stdout,
    stdout_contains,
    stderr_contains,
    walk_stderr_contains,
    proc,
):
    """The expectations an observed run misses, and whether a frame was one.

    Both backends answer the same file with the same streams, so the
    comparison is one function and what differs between them is how the
    program was made to run.
    """
    problems = []
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
    missing_walk = False
    for needle in stdout_contains:
        if needle not in proc.stdout:
            problems.append(f"missing stdout: {needle!r}")
    for needle in stderr_contains:
        if needle not in proc.stderr:
            problems.append(f"missing stderr: {needle!r}")
            if needle in walk_stderr_contains:
                missing_walk = True
    return problems, missing_walk


def run(
    alcy: Path,
    case: Case,
    work: Path,
    env: dict[str, str],
    mode: str = "native",
    backend: str = "",
):
    """Builds the case's program and runs it.

    Returns `(ok, detail)`, or `(None, reason)` when a sweep does not
    apply to this case, which the runner reports as a note rather than a
    failure.
    """
    step = case.program
    if step is None:
        return True, ""
    if mode == "wasm":
        if step.skip_wasm:
            return None, step.skip_wasm
        if WASM_INTERPRETER is None:
            return False, "bun is not on PATH, so the module cannot be run"
    if mode == "qemu":
        if step.skip_qemu:
            return None, step.skip_qemu
        if QEMU is None or SYSTEM_CLANG is None:
            return False, "qemu-riscv64 and clang are needed for a qemu run"
    sanitize = mode == "sanitized"
    if sanitize:
        if step.skip_sanitize:
            return None, step.skip_sanitize
        if is_freestanding(work):
            # A freestanding program defines its own entry and links no C
            # runtime, so it runs uninstrumented rather than not at all.
            sanitize = False

    expect = step.expect
    stderr_contains = list(expect.stderr_contains) if expect else []
    # The walk is a property of the runtime the program runs on. The
    # direct wasm backend writes the message and its site and exits, so a
    # module owes no frame even on a host whose frame pointer is a chain.
    if (
        expect is not None
        and mode != "wasm"
        and (mode == "qemu" or host_walks_frames())
    ):
        stderr_contains += list(expect.walk_stderr_contains)

    is_package = (work / "alcy.toml").is_file()
    if not is_package and not (work / "main.al").is_file():
        return False, "no main.al found"
    target = "." if is_package else "main.al"
    command = "build" if is_package else "compile"
    exe_name = "main_exe.exe" if os.name == "nt" else "main_exe"
    extra_args = list(step.build_args)
    expected_input = step.input
    problems = []
    missing_walk = False
    exe_path = None

    if mode == "wasm":
        # A module is the whole artifact the direct backend writes: the
        # interpreter supplies the runtime the executable would link.
        module = work / WASM_MODULE
        proc = subprocess.run(
            [
                str(alcy),
                command,
                target,
                *extra_args,
                f"--target={step.target or WASM_TARGET}",
                f"--backend={step.backend or WASM_BACKEND}",
                "-o",
                WASM_MODULE,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=work,
            env=env,
        )
        if proc.returncode != 0:
            return False, (
                f"alcy {command} for wasm failed: exit={proc.returncode}\n"
                f"--- build stdout ---\n{proc.stdout}"
                f"--- build stderr ---\n{proc.stderr}"
            )
        if not module.is_file():
            return False, (
                f"alcy {command} succeeded but '{WASM_MODULE}' is missing\n"
                f"--- build stdout ---\n{proc.stdout}"
                f"--- build stderr ---\n{proc.stderr}"
            )
        try:
            proc = subprocess.run(
                [WASM_INTERPRETER, str(WASM_SHIM), str(module)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=work,
                input=expected_input,
                env=env,
            )
        except OSError as e:
            return False, f"cannot run '{WASM_MODULE}': {e}"
    elif mode == "qemu":
        # The backend owns the freestanding entry; the CLI's linker driver
        # supplies the system linker, so this exercises the same emission
        # path an executable build uses.
        exe = work / "main_rv"
        link_args = [
            f"--target={RISCV_TARGET}",
            "-fuse-ld=lld",
            "-nostdlib",
            "-static",
            "-Wl,-e,_start",
        ]
        argv = [
            str(alcy),
            command,
            target,
            *extra_args,
            f"--target={RISCV_TARGET}",
            f"--backend={RISCV_BACKEND}",
            "--freestanding",
            "--emit=executable",
            f"--linker={SYSTEM_CLANG}",
            *(f"--link-args={arg}" for arg in link_args),
            "-o",
            str(exe),
        ]
        proc = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=work,
            env=env,
        )
        if proc.returncode != 0:
            return False, (
                f"alcy {command} for riscv64 failed: exit={proc.returncode}\n"
                f"--- build stdout ---\n{proc.stdout}"
                f"--- build stderr ---\n{proc.stderr}"
            )
        try:
            proc = subprocess.run(
                [QEMU, str(exe)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=work,
                input=expected_input,
                env=env,
                timeout=30,
            )
        except subprocess.TimeoutExpired:
            return False, "qemu-riscv64 timed out"
    else:
        if sanitize:
            proc, why = build_sanitized(
                alcy, work, is_package, work / exe_name, extra_args
            )
            if why:
                return False, f"{why}\n--- stderr ---\n{proc.stderr}"
        else:
            native_args = list(extra_args)
            if backend:
                native_args.append(f"--backend={backend}")
            proc = subprocess.run(
                [str(alcy), command, target, *native_args, "-o", exe_name],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=work,
                env=env,
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
        if not sanitize:
            fmt, foreign = foreign_imports(exe_path, is_freestanding(work))
            if foreign:
                return False, (
                    f"{fmt} image imports beyond the kernel: " + ", ".join(foreign)
                )
        try:
            run_env = dict(env)
            if sanitize:
                options = run_env.get("ASAN_OPTIONS", "").split(":")
                if "allocator_may_return_null=1" not in options:
                    options.append("allocator_may_return_null=1")
                run_env["ASAN_OPTIONS"] = ":".join(o for o in options if o)
            proc = subprocess.run(
                [str(exe_path)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=work,
                input=expected_input,
                env=run_env,
            )
        except OSError as e:
            return False, f"cannot execute '{exe_name}': {e}"

    stream_problems, missing_walk = compare_streams(
        expect.exit if expect else 0,
        expect.stdout if expect else None,
        list(expect.stdout_contains) if expect else [],
        stderr_contains,
        list(expect.walk_stderr_contains) if expect else [],
        proc,
    )
    problems += stream_problems
    if expect is not None:
        for needle in expect.contains:
            if needle not in (proc.stdout + proc.stderr):
                problems.append(f"missing output: {needle!r}")

    # A sanitizer writes its report to stderr and exits non-zero, which
    # the exit check may already have caught. A marker names a report
    # rather than the instrument: an instrumented build also warns about
    # a policy it applied, and the runtime's panic is what a case pins.
    for marker in (
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "AddressSanitizer:DEADLYSIGNAL",
        "Sanitizer CHECK failed",
        "runtime error:",
        "SEGV on unknown address",
    ):
        if marker in proc.stderr:
            problems.append(f"sanitizer report: {marker}")

    facts = image_facts(exe_path) if missing_walk and exe_path else ""
    if problems:
        detail = "; ".join(problems)
        detail += f"\n--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        if facts:
            detail += "\n" + facts
        return False, detail
    return True, ""
