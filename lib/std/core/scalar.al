// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the two text scalar types and their construction.
//
// A `char` holds an ASCII scalar and a `rune` holds a Unicode scalar.
// Both are compiler-known types, so the only way to build one from a
// wider integer is through an intrinsic whose precondition is checked
// here. The unchecked intrinsics are unsafe and stay inside this module.

pub unsafe intrinsic fn char_from_u8_unchecked(value: u8) -> char;

pub unsafe intrinsic fn rune_from_u32_unchecked(value: u32) -> rune;

// The char a byte names, or None when the byte is not ASCII.
pub fn char_from_u8(value: u8) -> Option<char> {
  if value > 0x7Fu8 {
    ret Option::None
  }
  ret Option::Some(unsafe { char_from_u8_unchecked(value) })
}

// The char a scalar names, or None when the scalar is not ASCII.
pub fn char_from_rune(value: rune) -> Option<char> {
  code := value as u32
  if code > 0x7Fu32 {
    ret Option::None
  }
  ret Option::Some(unsafe { char_from_u8_unchecked(code as u8) })
}

// The rune a 32-bit word names, or None for a surrogate or a value
// past the last scalar.
pub fn rune_from_u32(value: u32) -> Option<rune> {
  if value > 0x10FFFFu32 {
    ret Option::None
  }
  if value >= 0xD800u32 && value <= 0xDFFFu32 {
    ret Option::None
  }
  ret Option::Some(unsafe { rune_from_u32_unchecked(value) })
}

// The first scalar the bytes spell and the bytes it took, or the
// offset that did not decode. The caller keeps the rest of the view.
pub fn rune_from_utf8(bytes: &[u8]) -> Result<(rune, usize), Utf8Error> {
  if bytes.len() == 0 {
    ret Result::Err(Utf8Error { valid_up_to: 0 })
  }
  n := utf8_scalar_length(bytes, 0)
  if n == 0 {
    ret Result::Err(Utf8Error { valid_up_to: 0 })
  }
  mut code := 0u32
  if n == 1 {
    code = bytes[0] as u32
  } else {
    first := bytes[0]
    if n == 2 {
      code = (first & 0x1Fu8) as u32
    } else if n == 3 {
      code = (first & 0x0Fu8) as u32
    } else {
      code = (first & 0x07u8) as u32
    }
    mut i := 1 as usize
    while i < n {
      code = (code << 6) | ((bytes[i] & 0x3Fu8) as u32)
      i = i + 1
    }
  }
  ret Result::Ok((unsafe { rune_from_u32_unchecked(code) }, n))
}

// The byte length of the scalar that starts at `at` in a str, or zero
// when the bytes there do not spell one. A `str` is valid UTF-8, so
// this is only ever asked of a position the caller trusts.
fn str_scalar_length(s: str, at: usize) -> usize {
  total := str_len(s)
  if at >= total {
    ret 0
  }
  b := str_byte(s, at)
  if b < 0x80u8 {
    ret 1
  }
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
    ret 0
  }
  mut k := 1 as usize
  while k < want {
    if at + k >= total {
      ret 0
    }
    c := str_byte(s, at + k)
    mut fits := c >= 0x80u8 && c <= 0xBFu8
    if k == 1 {
      fits = c >= lo && c <= hi
    }
    if !fits {
      ret 0
    }
    k = k + 1
  }
  ret want
}

// The first scalar of a str and the bytes it took. A `str` is valid
// UTF-8, so the error arm only stands in for a broken view.
pub fn rune_from_str(s: str) -> Result<(rune, usize), Utf8Error> {
  total := str_len(s)
  if total == 0 {
    ret Result::Err(Utf8Error { valid_up_to: 0 })
  }
  n := str_scalar_length(s, 0)
  if n == 0 {
    ret Result::Err(Utf8Error { valid_up_to: 0 })
  }
  mut code := 0u32
  if n == 1 {
    code = str_byte(s, 0) as u32
  } else {
    first := str_byte(s, 0)
    if n == 2 {
      code = (first & 0x1Fu8) as u32
    } else if n == 3 {
      code = (first & 0x0Fu8) as u32
    } else {
      code = (first & 0x07u8) as u32
    }
    mut i := 1 as usize
    while i < n {
      code = (code << 6) | ((str_byte(s, i) & 0x3Fu8) as u32)
      i = i + 1
    }
  }
  ret Result::Ok((unsafe { rune_from_u32_unchecked(code) }, n))
}
