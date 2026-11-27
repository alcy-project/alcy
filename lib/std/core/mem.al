// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: memory and the size of a type.

// A value whose bytes can be copied to a new place and used there.
// The witness is structural: primitives, shared references and
// aggregates of copyable fields are copyable; a destructor, an
// anchor, or an exclusive reference takes it away. The compiler owns
// the spec, so no `impl` may name it.
pub intrinsic spec Copy {
};

// A value whose all-zero bytes spell a valid value. The witness is
// the set of primitives whose representation the compiler knows:
// bool, the integer and float widths, char and rune. A reference,
// an enum or a user type cannot be filled with zeros safely.
pub intrinsic spec Zeroable {
};

pub unsafe intrinsic fn memcopy<T>(dst: &mut T, src: &T, n: usize);

// Fills `count` elements of uninitialized storage with `byte`. The
// caller must ensure the bytes spell valid T values before reading
// them through `uninit_assume`, the same contract `alloc` carries.
pub unsafe intrinsic fn memset<T>(dst: &mut MaybeUninit<T>, byte: u8, count: usize);

// Raw pointer offset. The count is signed so one spelling moves both
// ways, and the result is in bounds exactly when the caller keeps it
// within the buffer the pointer names.
pub unsafe intrinsic fn ptr_offset<T>(ptr: *T, count: isize) -> *T;

pub unsafe intrinsic fn ptr_offset_mut<T>(ptr: *mut T, count: isize) -> *mut T;

pub intrinsic fn panic(msg: str) -> !;

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

// A module's own tests: `#[when(testing)]` keeps them out of a build,
// and `alcy test` gathers the `#[test] fn`s into a driver that runs them.
#[when(testing)] {
  #[test] fn byte_holds_one_byte() {
    assert(size_of::<u8>() == 1, "the unit of size is the byte")
  }

  #[test] fn i32_holds_four_bytes() {
    assert(size_of::<i32>() == 4, "four bytes, as the width says")
  }

  #[test] fn i64_holds_twice_an_i32() {
    assert(size_of::<i64>() == 2 * size_of::<i32>(), "the wider integer is twice as wide")
  }

  #[test] fn array_holds_all_its_elements() {
    assert(size_of::<[i32; 3]>() == 3 * size_of::<i32>(), "three elements, three times the element")
  }

  #[test] fn i32_sits_on_a_four_byte_boundary() {
    assert(align_of::<i32>() == 4, "the alignment matches the width")
  }

  #[test] fn byte_sits_on_a_one_byte_boundary() {
    assert(align_of::<u8>() == 1, "a byte needs no padding")
  }

  #[test] fn wrapping_add_rolls_a_byte_over() {
    assert(wrapping_add(u8::MAX, 1u8) == 0u8, "the sum is taken modulo 256")
  }

  #[test] fn wrapping_sub_rolls_a_byte_under() {
    assert(wrapping_sub(0u8, 1u8) == u8::MAX, "the difference is taken modulo 256")
  }

  #[test] fn wrapping_mul_keeps_the_low_byte() {
    assert(wrapping_mul(200u8, 2u8) == 144u8, "the product is taken modulo 256")
  }

  #[test] fn wrapping_add_rolls_an_i32_to_its_minimum() {
    assert(
      wrapping_add(i32::MAX, 1i32) == i32::MIN,
      "one past the maximum is the minimum",
    )
  }

  #[test] fn wrapping_add_of_values_that_fit_is_plain_addition() {
    assert(wrapping_add(2u8, 3u8) == 5u8, "no wrap happens below the limit")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
