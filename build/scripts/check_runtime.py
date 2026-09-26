#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Standalone check for the program runtime.

Compiles runtime/alcy_runtime.c with the system C compiler, links
print/panic/allocator driver programs against it, and asserts their
output, exit status, and allocation invariants.
"""

import os
import shutil
import subprocess
import sys
import tempfile

from pathlib import Path

from utils.paths import project_root_dir


def system_cc():
    cc = os.environ.get("CC")
    if cc:
        return cc
    for compiler in ["clang", "gcc", "cc"]:
        if shutil.which(compiler):
            return compiler
    return "cc"


def run(argv, **kwargs):
    proc = subprocess.run(argv, capture_output=True, text=True, **kwargs)
    return proc


def executable(name):
    suffix = ".exe" if sys.platform == "win32" else ""
    return f"{name}{suffix}"


def main():
    cc = system_cc()
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
            '  alcy_print("hi\\n", 3);\n'
            '  alcy_print("a:b\\n", 4);\n'
            '  alcy_println("see you", 7);\n'
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
        if proc.returncode != 0 or proc.stdout != "hi\na:b\nsee you\n":
            failures.append(
                f"print: exit={proc.returncode} stdout={proc.stdout!r} "
                f"stderr={proc.stderr!r}"
            )

        (tmpdir / "panic_main.c").write_text(
            '#include "alcy_runtime.h"\n'
            "int main(void) {\n"
            '  alcy_panic("boom\\n", 5);\n'
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

        (tmpdir / "alloc_main.c").write_text(
            '#include "alcy_runtime.h"\n'
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "#include <string.h>\n"
            "\n"
            "static int check_block(size_t size, size_t align, unsigned char value) {\n"
            "  unsigned char* ptr = (unsigned char*)alcy_alloc(size, align);\n"
            "  if (ptr == NULL) {\n"
            '    fprintf(stderr, "alloc(%zu, %zu) returned NULL\\n", size, align);\n'
            "    return 1;\n"
            "  }\n"
            "  if (((uintptr_t)ptr % align) != 0) {\n"
            '    fprintf(stderr, "alloc(%zu, %zu) returned a misaligned pointer\\n", size, align);\n'
            "    alcy_dealloc(ptr, size, align);\n"
            "    return 1;\n"
            "  }\n"
            "  if (size != 0) {\n"
            "    memset(ptr, value, size);\n"
            "    if (ptr[0] != value || ptr[size - 1] != value) {\n"
            '      fprintf(stderr, "alloc(%zu, %zu) memory was not writable\\n", size, align);\n'
            "      alcy_dealloc(ptr, size, align);\n"
            "      return 1;\n"
            "    }\n"
            "  }\n"
            "  alcy_dealloc(ptr, size, align);\n"
            "  return 0;\n"
            "}\n"
            "\n"
            "static int check_distinct(size_t size, size_t align) {\n"
            "  void* first = alcy_alloc(size, align);\n"
            "  void* second = alcy_alloc(size, align);\n"
            "  int bad = 0;\n"
            "  if (first == NULL || second == NULL) {\n"
            '    fprintf(stderr, "distinct alloc(%zu, %zu) returned NULL\\n", size, align);\n'
            "    bad = 1;\n"
            "  } else if (first == second) {\n"
            '    fprintf(stderr, "distinct alloc(%zu, %zu) returned the same pointer\\n", size, align);\n'
            "    bad = 1;\n"
            "  }\n"
            "  alcy_dealloc(first, size, align);\n"
            "  alcy_dealloc(second, size, align);\n"
            "  return bad;\n"
            "}\n"
            "\n"
            "int main(void) {\n"
            "  int bad = 0;\n"
            "  bad += check_block(32, 1, 0x11);\n"
            "  bad += check_block(16, 4, 0x22);\n"
            "  bad += check_block(0, 1, 0);\n"
            "  bad += check_distinct(32, 1);\n"
            "  bad += check_distinct(0, 1);\n"
            "  return bad == 0 ? 0 : 1;\n"
            "}\n"
        )
        alloc_exe = tmpdir / executable("alloc_test")
        proc = run(
            [
                cc,
                str(tmpdir / "alloc_main.c"),
                str(runtime_obj),
                "-I",
                str(runtime_dir),
                "-o",
                str(alloc_exe),
            ]
        )
        if proc.returncode != 0:
            print(f"FAIL link alloc driver:\n{proc.stderr}")
            return 1
        proc = run([str(alloc_exe)])
        if proc.returncode != 0:
            failures.append(
                f"alloc: exit={proc.returncode} stdout={proc.stdout!r} "
                f"stderr={proc.stderr!r}"
            )

    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1
    print("runtime: print/panic/alloc checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
