// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the limits of the fixed-width integer types.
//
// These are compile-time values on the type itself. A pointer-width
// limit is gated on the target's address width rather than inferred
// from the size of `usize`, which would only hold by coincidence.

impl i8 {
  pub const MIN: i8 := -128i8

  pub const MAX: i8 := 127i8
}

impl i16 {
  pub const MIN: i16 := -32768i16

  pub const MAX: i16 := 32767i16
}

impl i32 {
  pub const MIN: i32 := -2147483648i32

  pub const MAX: i32 := 2147483647i32
}

impl i64 {
  pub const MIN: i64 := -9223372036854775808i64

  pub const MAX: i64 := 9223372036854775807i64
}

impl u8 {
  pub const MIN: u8 := 0u8

  pub const MAX: u8 := 255u8
}

impl u16 {
  pub const MIN: u16 := 0u16

  pub const MAX: u16 := 65535u16
}

impl u32 {
  pub const MIN: u32 := 0u32

  pub const MAX: u32 := 4294967295u32
}

impl u64 {
  pub const MIN: u64 := 0u64

  pub const MAX: u64 := 18446744073709551615u64
}

impl usize {
  pub const MIN: usize := 0usize

  #[when(target.width == 64)]
  pub const MAX: usize := 18446744073709551615usize

  #[when(target.width == 32)]
  pub const MAX: usize := 4294967295usize
}

impl isize {
  pub const MIN: isize := -isize::MAX - 1

  #[when(target.width == 64)]
  pub const MAX: isize := 9223372036854775807isize

  #[when(target.width == 32)]
  pub const MAX: isize := 2147483647isize
}
