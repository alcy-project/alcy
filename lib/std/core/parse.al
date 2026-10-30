// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: reading a value out of its text.

// A type that can be read from its decimal text. The member is an
// associated function (ADR-0133), so the bound `T: Parse` is nameable
// and the call reads from the type: `i64::parse(text)`,
// `T::parse(text)`, `text.parse::<i64>()`.
pub spec Parse {
  fn parse(text: str) -> Option<Self>;
}

// Parses a decimal integer with an optional leading `+` or `-`.
// `None` when the bytes are not a number or the magnitude overflows
// `i64`, so no wrap is read as a value.
impl Parse for i64 {
  fn parse(text: str) -> Option<i64> {
    n := str_len(text)
    if n == 0 {
      ret Option::None
    }
    mut i := 0 as usize
    mut negative := false
    b := str_byte(text, 0)
    if b == 0x2du8 {
      negative = true
      i = 1
    } else if b == 0x2bu8 {
      i = 1
    }
    if i >= n {
      ret Option::None
    }
    mut limit := 9223372036854775807u64
    if negative {
      limit = 9223372036854775808u64
    }
    mut value := 0u64
    while i < n {
      d := str_byte(text, i)
      if d < 0x30u8 || d > 0x39u8 {
        ret Option::None
      }
      digit := (d - 0x30u8) as u64
      if value > (limit - digit) / 10u64 {
        ret Option::None
      }
      value = value * 10u64 + digit
      i = i + 1
    }
    if negative {
      if value == 9223372036854775808u64 {
        ret Option::Some(0i64 - 9223372036854775807i64 - 1i64)
      }
      ret Option::Some(0i64 - (value as i64))
    }
    ret Option::Some(value as i64)
  }
}

// Parses a decimal integer that must also fit `i32`.
impl Parse for i32 {
  fn parse(text: str) -> Option<i32> {
    match i64::parse(text) {
      Option::Some(v) => {
        if v < 0i64 - 2147483647i64 - 1i64 || v > 2147483647i64 {
          ret Option::None
        }
        ret Option::Some(v as i32)
      }
      Option::None => {
        ret Option::None
      }
    }
  }
}

// Parses an unsigned decimal integer, with an optional leading `+`.
// `None` when the bytes are not a number or the magnitude overflows
// `u64`.
impl Parse for u64 {
  fn parse(text: str) -> Option<u64> {
    n := str_len(text)
    if n == 0 {
      ret Option::None
    }
    mut i := 0 as usize
    if str_byte(text, 0) == 0x2bu8 {
      i = 1
    }
    if i >= n {
      ret Option::None
    }
    mut value := 0u64
    while i < n {
      d := str_byte(text, i)
      if d < 0x30u8 || d > 0x39u8 {
        ret Option::None
      }
      digit := (d - 0x30u8) as u64
      if value > (18446744073709551615u64 - digit) / 10u64 {
        ret Option::None
      }
      value = value * 10u64 + digit
      i = i + 1
    }
    ret Option::Some(value)
  }
}

// Parses a decimal integer that must also fit `u32`.
impl Parse for u32 {
  fn parse(text: str) -> Option<u32> {
    match u64::parse(text) {
      Option::Some(v) => {
        if v > 4294967295u64 {
          ret Option::None
        }
        ret Option::Some(v as u32)
      }
      Option::None => {
        ret Option::None
      }
    }
  }
}

// Parses a decimal floating point number with an optional sign and
// fractional part. An exponent is not read, so `1e10` is rejected.
// `None` when the bytes are not a number or the digits pass what a
// `u64` numerator holds.
impl Parse for f64 {
  fn parse(text: str) -> Option<f64> {
    n := str_len(text)
    if n == 0 {
      ret Option::None
    }
    mut i := 0 as usize
    mut negative := false
    b := str_byte(text, 0)
    if b == 0x2du8 {
      negative = true
      i = 1
    } else if b == 0x2bu8 {
      i = 1
    }
    if i >= n {
      ret Option::None
    }
    mut numerator := 0u64
    mut scale := 1u64
    mut seen := false
    mut point := false
    while i < n {
      d := str_byte(text, i)
      if d == 0x2eu8 {
        if point {
          ret Option::None
        }
        point = true
        i = i + 1
        continue
      }
      if d < 0x30u8 || d > 0x39u8 {
        ret Option::None
      }
      digit := (d - 0x30u8) as u64
      if numerator > (18446744073709551615u64 - digit) / 10u64 {
        ret Option::None
      }
      numerator = numerator * 10u64 + digit
      if point {
        if scale > 1844674407370955161u64 {
          ret Option::None
        }
        scale = scale * 10u64
      }
      seen = true
      i = i + 1
    }
    if !seen {
      ret Option::None
    }
    // The integer part and the fraction are converted apart: one
    // division of the whole numerator rounds twice, and a value just
    // past 2^53 then answers one step off (`"9007199254740993.0"` read
    // as 9007199254740994 while the literal reads 9007199254740992).
    // The integer part is exact, and an integral value keeps it.
    whole := numerator / scale
    rem := numerator % scale
    mut value := (whole as f64) + (rem as f64) / (scale as f64)
    if negative {
      value = 0.0f64 - value
    }
    ret Option::Some(value)
  }
}
