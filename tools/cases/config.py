#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""The one reader of `case.toml`.

A case names how the compiler is called in `[cli]` and, when the program
it builds can be executed, what that program answers in `[program]`.
Both steps carry their own expectation; neither subcommand is special.
Sweep sections hold a reason for skipping a case, not a boolean, so a
skip is a decision a reader can revisit.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

CLI_KEYS = frozenset({"subcommand", "args", "env", "expect"})
PROGRAM_KEYS = frozenset(
    {"build_args", "input", "target", "backend", "interpreter", "expect", "sweep"}
)
EXPECT_KEYS = frozenset(
    {
        "exit",
        "contains",
        "not_contains",
        "stdout",
        "stdout_contains",
        "stderr_contains",
        "walk_stderr_contains",
    }
)
SWEEP_KEYS = frozenset({"sanitize", "wasm"})


class CaseError(Exception):
    """A case whose description cannot be answered by any runner."""


@dataclass(frozen=True)
class Expect:
    exit: int | str
    contains: tuple[str, ...] = ()
    not_contains: tuple[str, ...] = ()
    stdout: str | None = None
    stdout_contains: tuple[str, ...] = ()
    stderr_contains: tuple[str, ...] = ()
    walk_stderr_contains: tuple[str, ...] = ()


@dataclass(frozen=True)
class CliStep:
    subcommand: str
    args: tuple[str, ...] = ()
    env: dict[str, str] = field(default_factory=dict)
    expect: Expect | None = None


@dataclass(frozen=True)
class ProgramStep:
    build_args: tuple[str, ...] = ()
    input: str = ""
    target: str = ""
    backend: str = ""
    interpreter: str = ""
    expect: Expect | None = None
    skip_sanitize: str = ""
    skip_wasm: str = ""


@dataclass(frozen=True)
class Case:
    path: Path
    cli: CliStep | None = None
    program: ProgramStep | None = None
    freestanding: bool = False

    @property
    def name(self) -> str:
        return self.path.name


def _unknown(where: str, keys, allowed: frozenset[str]) -> None:
    extra = sorted(set(keys) - allowed)
    if extra:
        raise CaseError(f"{where}: unknown key(s): {', '.join(extra)}")


def _strings(where: str, value, name: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        raise CaseError(f"{where}: '{name}' must be a list of strings")
    return tuple(value)


def _expect(where: str, value) -> Expect:
    if not isinstance(value, dict):
        raise CaseError(f"{where}: 'expect' must be a table")
    _unknown(where, value, EXPECT_KEYS)
    exit_value = value.get("exit")
    if not isinstance(exit_value, int) and exit_value != "non-zero":
        raise CaseError(f"{where}: 'exit' must be an integer or 'non-zero'")
    stdout = value.get("stdout")
    if stdout is not None and not isinstance(stdout, str):
        raise CaseError(f"{where}: 'stdout' must be a string")
    return Expect(
        exit=exit_value,
        contains=_strings(where, value.get("contains"), "contains"),
        not_contains=_strings(where, value.get("not_contains"), "not_contains"),
        stdout=stdout,
        stdout_contains=_strings(
            where, value.get("stdout_contains"), "stdout_contains"
        ),
        stderr_contains=_strings(
            where, value.get("stderr_contains"), "stderr_contains"
        ),
        walk_stderr_contains=_strings(
            where, value.get("walk_stderr_contains"), "walk_stderr_contains"
        ),
    )


def read_case(path: Path) -> Case:
    """Reads one case directory, refusing a description no runner can answer."""
    config = path / "case.toml"
    if not config.is_file():
        raise CaseError(f"{path}: no case.toml")
    with open(config, "rb") as f:
        data = tomllib.load(f)
    _unknown(str(config), data, frozenset({"cli", "program"}))

    cli = None
    if "cli" in data:
        step = data["cli"]
        if not isinstance(step, dict):
            raise CaseError(f"{config}: 'cli' must be a table")
        _unknown(f"{config}: [cli]", step, CLI_KEYS)
        subcommand = step.get("subcommand")
        if not isinstance(subcommand, str):
            raise CaseError(f"{config}: [cli] needs a 'subcommand'")
        env = step.get("env", {})
        if not isinstance(env, dict) or not all(
            isinstance(k, str) and isinstance(v, str) for k, v in env.items()
        ):
            raise CaseError(f"{config}: [cli.env] must map strings to strings")
        cli = CliStep(
            subcommand=subcommand,
            args=_strings(f"{config}: [cli]", step.get("args"), "args"),
            env=dict(env),
            expect=_expect(f"{config}: [cli]", step.get("expect")),
        )

    program = None
    if "program" in data:
        step = data["program"]
        if not isinstance(step, dict):
            raise CaseError(f"{config}: 'program' must be a table")
        _unknown(f"{config}: [program]", step, PROGRAM_KEYS)
        sweeps = step.get("sweep", {})
        if not isinstance(sweeps, dict):
            raise CaseError(f"{config}: [program.sweep] must be a table")
        _unknown(f"{config}: [program.sweep]", sweeps, SWEEP_KEYS)
        skip = {}
        for name, entry in sweeps.items():
            if not isinstance(entry, dict) or not isinstance(entry.get("skip"), str):
                raise CaseError(
                    f"{config}: [program.sweep.{name}] needs a 'skip' reason"
                )
            skip[name] = entry["skip"]
        program = ProgramStep(
            build_args=_strings(
                f"{config}: [program]", step.get("build_args"), "build_args"
            ),
            input=step.get("input", ""),
            target=step.get("target", ""),
            backend=step.get("backend", ""),
            interpreter=step.get("interpreter", ""),
            expect=_expect(f"{config}: [program]", step.get("expect")),
            skip_sanitize=skip.get("sanitize", ""),
            skip_wasm=skip.get("wasm", ""),
        )

    if cli is None and program is None:
        raise CaseError(f"{config}: a case needs [cli] or [program]")
    toolchain = path / ".alcy" / "toolchain.toml"
    freestanding = False
    if toolchain.is_file():
        with open(toolchain, "rb") as f:
            freestanding = bool(tomllib.load(f).get("freestanding", False))
    return Case(path=path, cli=cli, program=program, freestanding=freestanding)
