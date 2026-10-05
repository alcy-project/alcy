// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: memory, the process, and the size of a type.

pub unsafe intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

pub intrinsic fn panic(msg: str) -> !;

intrinsic fn sys_write(fd: i32, buf: str);

pub fn print(msg: str) {
  sys_write(1, msg)
}

pub fn println(msg: str) {
  sys_write(1, msg)
  sys_write(1, "\n")
}

pub intrinsic fn size_of<T>() -> usize;

pub intrinsic fn align_of<T>() -> usize;
