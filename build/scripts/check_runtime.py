#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

"""Standalone check for the program runtime.

Compiles runtime/alcy_runtime.c with the system C compiler, links
print/panic driver programs against it, and asserts stdout content,
stderr content, and the abort exit status.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

from utils.paths import project_root_dir


def run(argv, **kwargs):
    proc = subprocess.run(argv, capture_output=True, text=True, **kwargs)
    return proc


def executable(name):
    suffix = ".exe" if sys.platform == "win32" else ""
    return f"{name}{suffix}"


def main():
    cc = "cc"
    runtime_dir = project_root_dir / "runtime"
    failures = []

    with tempfile.TemporaryDirectory(prefix="alcy_runtime_test") as tmp:
        tmpdir = Path(tmp)
        runtime_obj = tmpdir / "alcy_runtime.o"
        proc = run(
            [
                cc,
                "-std=c11",
                "-Wall",
                "-Werror",
                "-c",
                str(runtime_dir / "alcy_runtime.c"),
                "-o",
                str(runtime_obj),
            ]
        )
        if proc.returncode != 0:
            print(f"FAIL compile runtime:\n{proc.stderr}")
            return 1

        (tmpdir / "print_main.c").write_text(
            '#include "alcy_runtime.h"\n'
            "int main(void) {\n"
            '  alcy_print("hi");\n'
            '  alcy_print("a:b");\n'
            "  return 0;\n"
            "}\n"
        )
        print_exe = tmpdir / executable("print_test")
        proc = run(
            [
                cc,
                str(tmpdir / "print_main.c"),
                str(runtime_obj),
                "-I",
                str(runtime_dir),
                "-o",
                str(print_exe),
            ]
        )
        if proc.returncode != 0:
            print(f"FAIL link print driver:\n{proc.stderr}")
            return 1
        proc = run([str(print_exe)])
        if proc.returncode != 0 or proc.stdout != "hi\na:b\n":
            failures.append(
                f"print: exit={proc.returncode} stdout={proc.stdout!r} "
                f"stderr={proc.stderr!r}"
            )

        (tmpdir / "panic_main.c").write_text(
            '#include "alcy_runtime.h"\n'
            "int main(void) {\n"
            '  alcy_panic("boom");\n'
            "  return 0;\n"
            "}\n"
        )
        panic_exe = tmpdir / executable("panic_test")
        proc = run(
            [
                cc,
                str(tmpdir / "panic_main.c"),
                str(runtime_obj),
                "-I",
                str(runtime_dir),
                "-o",
                str(panic_exe),
            ]
        )
        if proc.returncode != 0:
            print(f"FAIL link panic driver:\n{proc.stderr}")
            return 1
        proc = run([str(panic_exe)])

        # Windows: abort() exits positive (e.g., 3221226505 / 0xC0000409);
        # Unix: SIGABRT exits negative (-6).
        abort_exit = proc.returncode
        is_windows = sys.platform == "win32"
        expected_abort = -6 if not is_windows else 3221226505
        if (
            abs(abort_exit) == abs(expected_abort) or abort_exit == expected_abort
        ) and (proc.stderr.splitlines()[0] == "boom" if proc.stderr else False):
            pass  # ok
        else:
            failures.append(
                f"panic: exit={proc.returncode} stdout={proc.stdout!r} "
                f"stderr={proc.stderr!r}"
            )

    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1
    print("runtime: print/panic checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
