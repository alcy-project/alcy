# ADR-0018: Compile Builds a File, Build Builds a Package

- Subject: the compiler
- Status: Accepted
- Date: 2026-09-28

## Context

`build`, `check`, and `run` each sniffed the target's extension to
decide between a single file and a package. The sniff lived in three
places, and `build` carried options for two different jobs: package
settings on one side, single-file reproducibility on the other.

## Decision

- `compile` is new and takes a single source file, plus `--stdin`
  (moved from `check`) for one virtual file. It owns `-o`, `--emit`,
  `--linker`, and `--release`.
- `build` and `run` take a package directory only. A file target is a
  validation error pointing at `compile`.
- `check` takes a package directory, or one file through `--file`.
- `--stdin` without `-o` is a validation error: the pipe names no
  file, so an unnamed output has no extension to replace.

## Consequences

`check --stdin` is gone; editors wanting a quick check read from a
process substitution or a temporary file instead. The pipeline keeps
`build_single_file` and `build_single_root`, so only the CLI and its
callers observe the split.
