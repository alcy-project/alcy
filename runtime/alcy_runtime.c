// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Minimal program runtime, compiled with the user program and linked
// against libc. `print` lowers directly to the write syscall through
// libc's unbuffered wrapper; this migrates to an ordinary core
// function once FFI lands.

#include "alcy_runtime.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define write _write
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
typedef intptr_t ssize_t;
#else
#include <unistd.h>
#endif

// Writes the whole buffer, retrying short writes.
static void write_all(int fd, const char* data, size_t len) {
  size_t written = 0;
  while (written < len) {
    const ssize_t count = write(fd, data + written, len - written);
    if (count <= 0) {
      return;
    }
    written += (size_t)count;
  }
}

void alcy_print(const char* message, size_t len) {
  write_all(STDOUT_FILENO, message == NULL ? "" : message, len);
}

void alcy_println(const char* message, size_t len) {
  if (message == NULL) {
    message = "";
  }
  write_all(STDOUT_FILENO, message, len);
  write_all(STDOUT_FILENO, "\n", 1);
}

void alcy_panic(const char* message, size_t len) {
  if (message == NULL) {
    message = "";
  }
  write_all(STDERR_FILENO, message, len);
  abort();
}

// Raw file-descriptor write backing core `print`. Retries short
// writes like the legacy helpers; gives up (rather than spinning)
// when the descriptor stops accepting bytes.
void alcy_sys_write(int fd, const char* buf, size_t len) {
  write_all(fd, buf == NULL ? "" : buf, len);
}
