// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

// Runtime surface linked into every alcy program. `str` values are
// pointers to NUL-terminated bytes (matching the globals codegen
// emits); both entry points treat a null pointer as an empty string.

#pragma once

// Writes the message plus a trailing newline to stdout.
void alcy_print(const char* message);

// Writes the message plus a trailing newline to stderr, then aborts.
void alcy_panic(const char* message);
