// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: memory, the process, and the size of a type.

pub unsafe intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

// Raw pointer offset. The count is signed so one spelling moves both
// ways, and the result is in bounds exactly when the caller keeps it
// within the buffer the pointer names.
pub unsafe intrinsic fn ptr_offset<T>(ptr: *T, count: isize) -> *T;

pub unsafe intrinsic fn ptr_offset_mut<T>(ptr: *mut T, count: isize) -> *mut T;

pub intrinsic fn panic(msg: str) -> !;

intrinsic fn sys_write(fd: i32, buf: str);

pub intrinsic fn size_of<T>() -> usize;

pub intrinsic fn align_of<T>() -> usize;

// Runs the value's destructor in place. The caller guarantees the
// value behind `p` is initialized and owns what it holds; the value is
// consumed, so the caller must not read it again.
pub unsafe intrinsic fn drop_in_place<T>(p: &mut T);
