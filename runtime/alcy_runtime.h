// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Runtime surface linked into every alcy program. `str` values are
// pointers to NUL-terminated bytes (matching the globals codegen
// emits); both entry points treat a null pointer as an empty string.

#pragma once

// Writes the message plus a trailing newline to stdout.
void alcy_print(const char* message);

// Writes the message plus a trailing newline to stderr, then aborts.
void alcy_panic(const char* message);
