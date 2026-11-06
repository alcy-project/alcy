// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/fmt`: how a type of one's own joins `{}`.

// `{}` reaches `display` for a type that is not one of the builtin
// convertibles. The implementation writes the prefix of its rendering
// that fits into `out` and returns the total length, so an expansion
// sizing a growable buffer learns the size from an empty slice. One
// placeholder may run the method twice (once to measure, once to
// write), so it must not have effects beyond `out`.
pub spec Display {
  fn display(self: &Self, out: &mut [u8]) -> usize;
}

// The owned string formats as its bytes, like `str` does, so a
// `format` result reaches `{}` without a detour through `as_str`.
impl Display for String {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    bytes := self.as_bytes()
    n := bytes.len()
    if n == 0 || out.len() == 0 {
      ret n
    }
    mut count := n
    if count > out.len() {
      count = out.len()
    }
    unsafe { memcopy(slice_at_mut(out, 0), slice_at(bytes, 0), count) }
    ret n
  }
}

test {
  test a_rendering_is_the_bytes_of_the_string() {
    hello := String::from("hello")
    mut out := String::from("........")
    n := hello.display(out.as_mut_slice())
    assert(n == 5, "the count is the length of the rendering")
    assert(out.byte_slice(0, 5) == "hello", "the bytes land at the front")
    assert(out.byte(5) == 0x2eu8, "the rest of the slice is untouched")
  }

  test an_empty_string_renders_as_no_bytes() {
    empty := String::from("")
    mut out := String::from("..")
    n := empty.display(out.as_mut_slice())
    assert(n == 0, "there is nothing to render")
    assert(out.byte(0) == 0x2eu8, "the slice is untouched")
  }

  test an_empty_slice_is_the_measurement_pass() {
    hello := String::from("hello")
    mut nothing := String::with_capacity(0)
    n := hello.display(nothing.as_mut_slice())
    assert(n == 5, "the count comes back with no room to write into")
  }

  test a_short_slice_takes_the_prefix_and_the_count_still_names_the_whole() {
    hello := String::from("hello")
    mut out := String::from("...")
    n := hello.display(out.as_mut_slice())
    assert(n == 5, "the count is not truncated")
    assert(out.byte_slice(0, 3) == "hel", "the slice takes what fits")
  }

  test a_zero_byte_is_a_byte_of_the_rendering() {
    mut bytes := String::new()
    bytes.push(0x61u8)
    bytes.push(0u8)
    bytes.push(0x62u8)
    mut out := String::from("...")
    n := bytes.display(out.as_mut_slice())
    assert(n == 3, "the rendering is three bytes")
    assert(out.byte(1) == 0u8, "the zero byte is copied like any other")
    assert(out.byte(2) == 0x62u8, "the byte after it follows")
  }

  test a_multibyte_character_counts_as_its_bytes() {
    mut e_acute := String::new()
    e_acute.push(0xc3u8)
    e_acute.push(0xa9u8)
    mut out := String::from("..")
    n := e_acute.display(out.as_mut_slice())
    assert(n == 2, "one character, two bytes")
    assert(out.byte(0) == 0xc3u8, "the first byte of the encoding lands")
    assert(out.byte(1) == 0xa9u8, "the second byte of the encoding lands")
  }

  test a_measure_pass_then_a_write_pass_fills_the_buffer_it_sized() {
    text := String::from("hello")
    mut nothing := String::with_capacity(0)
    n := text.display(nothing.as_mut_slice())
    mut out := String::with_capacity(n)
    unsafe { out.set_len(n) }
    written := text.display(out.as_mut_slice())
    assert(written == n, "the writing pass agrees with the measuring pass")
    assert(out.as_str() == "hello", "the buffer holds the rendering")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
