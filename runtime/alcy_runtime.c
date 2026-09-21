// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

// Minimal program runtime, compiled with the user program and linked
// against libc. `print` lowers directly to the write syscall through
// libc's unbuffered wrapper; this migrates to an ordinary core
// function once FFI lands.

#include "alcy_runtime.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

void alcy_print(const char* message) {
  if (message == NULL) {
    message = "";
  }
  write_all(STDOUT_FILENO, message, strlen(message));
  write_all(STDOUT_FILENO, "\n", 1);
}

void alcy_panic(const char* message) {
  if (message == NULL) {
    message = "";
  }
  write_all(STDERR_FILENO, message, strlen(message));
  write_all(STDERR_FILENO, "\n", 1);
  abort();
}
