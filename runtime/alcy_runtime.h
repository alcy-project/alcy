// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Runtime surface linked into every alcy program. `str` values are
// length-driven (ptr, len) pairs matching the fat-pointer layout the
// compiler emits; messages need no NUL terminator.

#pragma once

#include <stddef.h>

// Writes exactly `len` bytes of the message to stdout.
void alcy_print(const char* message, size_t len);

// Writes exactly `len` bytes of the message plus a trailing newline
// to stdout.
void alcy_println(const char* message, size_t len);

// Writes exactly `len` bytes of the message to stderr, then aborts.
void alcy_panic(const char* message, size_t len);
