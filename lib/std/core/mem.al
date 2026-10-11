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

// Reads the value at `p` and leaves the place uninitialized. The
// caller owns the result and must not use or end `p` again.
pub unsafe intrinsic fn read<T>(p: *T) -> T;

// The explicitly wrapping forms of `+`, `-`, and `*`: the result is
// the mathematical one reduced modulo 2^N, whatever the build's
// overflow policy is. A hash that relies on the wrap says so through
// these.
pub intrinsic fn wrapping_add<T>(a: T, b: T) -> T;

pub intrinsic fn wrapping_sub<T>(a: T, b: T) -> T;

pub intrinsic fn wrapping_mul<T>(a: T, b: T) -> T;
