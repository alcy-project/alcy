// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/unicode`: UTF-8 as Unicode's decoding algorithms read it.

// Whether the bytes are well-formed UTF-8. Decoding happens the way
// the same algorithm declares it in `core::is_utf8`; this is a thin
// name over the byte-level predicate.
pub fn valid(bytes: &[u8]) -> bool {
  ret is_utf8(bytes)
}

#[when(testing)] {
  #[test] fn empty_bytes_are_valid() {
    bytes := &[0u8; 0]
    assert(valid(bytes), "no bytes decode")
  }

  #[test] fn ascii_is_valid() {
    assert(valid(&[0x68u8, 0x65u8, 0x6cu8, 0x6cu8, 0x6fu8]), "every byte is a scalar of its own")
  }

  #[test] fn multibyte_is_valid() {
    assert(
      valid(&[0xe6u8, 0x97u8, 0xa5u8, 0xe6u8, 0x9cu8, 0xacu8, 0xe8u8, 0xa9u8, 0x9eu8]),
      "three scalars of three bytes each",
    )
  }

  #[test] fn truncated_sequence_is_not_valid() {
    assert(!valid(&[0xe1u8, 0x80u8]), "a lead byte alone decodes nothing")
  }

  #[test] fn sequence_that_starts_on_a_continuation_is_not_valid() {
    assert(!valid(&[0xa5u8, 0xe6u8, 0x97u8, 0xa5u8]), "a continuation can never open a scalar")
  }

  #[test] fn first_scalar_alone_is_valid() {
    assert(valid(&[0xe6u8, 0x97u8, 0xa5u8]), "the lead and continuations agree")
  }

  #[test] fn stray_continuation_is_not_valid() {
    assert(!valid(&[0x80u8]), "a continuation with no lead is not a scalar")
  }

  #[test] fn overlong_form_is_not_valid() {
    assert(!valid(&[0xf0u8, 0x80u8, 0x80u8]), "an overlong start is not a scalar")
  }

  #[test] fn surrogate_is_not_valid() {
    assert(!valid(&[0xedu8, 0xa0u8, 0x80u8]), "no scalar has a surrogate's shape")
  }

  #[test] fn past_the_last_scalar_is_not_valid() {
    assert(!valid(&[0xf4u8, 0x90u8, 0x80u8, 0x80u8]), "one past the last scalar is none")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
