// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/unicode`: UTF-8 as Unicode's decoding algorithms read it.

// Whether the bytes are well-formed UTF-8: every scalar decodes, no
// surrogate is encoded, and no sequence is overlong or truncated. The
// ranges below are the decoding algorithms' own (UAX #3), the same rule
// `str::count_chars` counts an ill-formed subsequence by.
pub fn valid(text: str) -> bool {
  mut i := 0 as usize
  total := str_len(text)
  while i < total {
    b := str_byte(text, i)
    if b < 0x80u8 {
      i = i + 1
      continue
    }
    // The length a lead byte asks for, and the range its first
    // continuation byte may fall in. Zero means the byte is not a lead
    // byte at all.
    mut want := 0 as usize
    mut lo := 0x80u8
    mut hi := 0xBFu8
    if b >= 0xC2u8 && b <= 0xDFu8 {
      want = 2
    } else if b == 0xE0u8 {
      want = 3
      lo = 0xA0u8
    } else if b >= 0xE1u8 && b <= 0xECu8 {
      want = 3
    } else if b == 0xEDu8 {
      want = 3
      hi = 0x9Fu8
    } else if b >= 0xEEu8 && b <= 0xEFu8 {
      want = 3
    } else if b == 0xF0u8 {
      want = 4
      lo = 0x90u8
    } else if b >= 0xF1u8 && b <= 0xF3u8 {
      want = 4
    } else if b == 0xF4u8 {
      want = 4
      hi = 0x8Fu8
    }
    if want == 0 {
      ret false
    }
    mut k := 1 as usize
    while k < want {
      if i + k >= total {
        ret false
      }
      c := str_byte(text, i + k)
      mut fits := c >= 0x80u8 && c <= 0xBFu8
      if k == 1 {
        fits = c >= lo && c <= hi
      }
      if !fits {
        ret false
      }
      k = k + 1
    }
    i = i + want
  }
  ret true
}

// A module's own tests: `alcy test` splices the block into a driver and
// runs each entry as a process of its own.
test {
  test empty_text_is_valid() {
    assert(valid(""), "no bytes decode")
  }

  test ascii_is_valid() {
    assert(valid("hello"), "every byte is a scalar of its own")
  }

  test multibyte_is_valid() {
    assert(valid("日本語"), "three scalars of three bytes each")
  }

  test a_truncated_sequence_is_not_valid() {
    assert(!valid(str_slice("日本", 0, 1)), "a lead byte alone decodes nothing")
  }

  test a_sequence_that_starts_on_a_continuation_is_not_valid() {
    assert(!valid(str_slice("日本", 1, 3)), "no scalar has a continuation lead")
  }

  test the_first_scalar_alone_is_valid() {
    assert(valid(str_slice("日本語", 0, 3)), "the slice stops on a boundary")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
