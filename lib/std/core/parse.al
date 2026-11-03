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

// The count of bits a value needs: 0 for zero, one past the index of
// its highest set bit otherwise.
fn u64_bit_count(value: u64) -> i32 {
  mut count := 0i32
  mut rest := value
  while rest != 0u64 {
    rest = rest >> 1u64
    count = count + 1i32
  }
  ret count
}

// `mant * 2^exponent`, moving the binary point only. A power of two of
// either sign is exact, so the significand keeps the bits rounding chose.
fn times_power_of_two(mant: u64, exponent: i32) -> f64 {
  ret scale_by_power_of_two(mant as f64, exponent)
}

// The same scaling for a value that already carries its quotient.
fn scale_by_power_of_two(base: f64, exponent: i32) -> f64 {
  mut value := base
  mut e := exponent
  while e > 0i32 {
    value = value * 2.0f64
    e = e - 1i32
  }
  while e < 0i32 {
    value = value / 2.0f64
    e = e + 1i32
  }
  ret value
}

// Negation that keeps the sign of a zero, so `"-0.0"` reads as the
// literal does.
fn apply_sign(value: f64, negative: bool) -> f64 {
  if !negative {
    ret value
  }
  ret value * (0.0f64 - 1.0f64)
}

// Parses a decimal floating point number with an optional sign,
// fractional part and exponent, the spelling a float literal takes, so
// the exponent form the formatter writes reads back. `None` when the
// bytes are not a number, when the exponent is empty, or when the digits,
// their scale and the exponent's power of ten pass what a `u64` holds; a
// magnitude past that budget is refused rather than read approximately.
// Otherwise the result is the nearest `f64` to the written decimal, so
// text and a literal of the same digits agree bit for bit.
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
      if d == 0x65u8 || d == 0x45u8 {
        break
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
    // An exponent moves the binary point the digits settle, so the
    // quotient below scales the digits once, the same way a fraction
    // does. Its magnitude is bounded first, which keeps the scaling
    // loop short whatever the text asks for.
    mut power := 0i32
    mut power_negative := false
    if i < n {
      i = i + 1
      if i < n {
        s := str_byte(text, i)
        if s == 0x2du8 {
          power_negative = true
          i = i + 1
        } else if s == 0x2bu8 {
          i = i + 1
        }
      }
      mut digits := 0
      while i < n {
        d := str_byte(text, i)
        if d < 0x30u8 || d > 0x39u8 {
          ret Option::None
        }
        if power > 4096i32 {
          ret Option::None
        }
        power = power * 10i32 + ((d - 0x30u8) as i32)
        digits = digits + 1
        i = i + 1
      }
      if digits == 0 {
        ret Option::None
      }
      if power_negative {
        power = 0i32 - power
      }
    }
    if numerator == 0u64 {
      ret Option::Some(apply_sign(0.0f64, negative))
    }
    // A positive power folds into the numerator as many factors of five,
    // which a `u64` holds longer than factors of ten, and the twos ride
    // the binary exponent afterwards, where scaling is exact.
    mut binary_power := 0i32
    while power > 0i32 {
      if numerator > 3689348814741910323u64 {
        ret Option::None
      }
      numerator = numerator * 5u64
      binary_power = binary_power + 1i32
      power = power - 1i32
    }
    while power < 0i32 {
      if scale > 1844674407370955161u64 {
        ret Option::None
      }
      scale = scale * 10u64
      power = power + 1i32
    }
    // A numerator the significand holds exactly is divided once, and
    // IEEE division of two exact operands is the nearest value already.
    if numerator <= 9007199254740992u64 {
      quotient := scale_by_power_of_two((numerator as f64) / (scale as f64), binary_power)
      ret Option::Some(apply_sign(quotient, negative))
    }
    // A longer numerator needs the quotient's bits: converting the
    // integer part and the fraction apart rounds twice, and
    // `"0.9876543210987654321"` then lands one step from the literal.
    // The significand is assembled here with exact integer arithmetic
    // and rounded once, to nearest and to even on a tie.
    mut mantissa := numerator / scale
    mut remainder := numerator % scale
    mut exponent := 0i32
    mut sticky := false
    while u64_bit_count(mantissa) > 54i32 {
      if (mantissa & 1u64) != 0u64 {
        sticky = true
      }
      mantissa = mantissa >> 1u64
      exponent = exponent + 1i32
    }
    while u64_bit_count(mantissa) < 54i32 && remainder != 0u64 {
      // Doubling the remainder can pass 2^64, which the carry reads: the
      // fraction stays below one, so the wrapped subtraction lands right.
      carry := (remainder >> 63u64) != 0u64
      remainder = remainder << 1u64
      bit := carry || remainder >= scale
      if bit {
        remainder = wrapping_sub(remainder, scale)
      }
      mantissa = mantissa << 1u64
      if bit {
        mantissa = mantissa | 1u64
      }
      exponent = exponent - 1i32
    }
    if remainder != 0u64 {
      sticky = true
    }
    if u64_bit_count(mantissa) == 54i32 {
      // One guard bit plus the sticky tail; a tie goes to the even
      // significand.
      mut kept := mantissa >> 1u64
      if (mantissa & 1u64) != 0u64 && (sticky || (kept & 1u64) != 0u64) {
        kept = kept + 1u64
      }
      mantissa = kept
      exponent = exponent + 1i32
    }
    ret Option::Some(apply_sign(times_power_of_two(mantissa, exponent + binary_power), negative))
  }
}

// A module's own tests: 'alcy test' splices the block into a driver and
// runs each entry as a process of its own.
test {
  test an_integer_reads_its_decimal_text() {
    assert(
      i64::parse("42").or_panic_with_message("42 is a number") == 42i64,
      "two digits read as twenty-two plus twenty",
    )
    assert(
      i64::parse("7").or_panic_with_message("7 is a number") == 7i64,
      "a single digit reads on its own",
    )
  }

  test a_leading_plus_is_read_and_not_counted() {
    assert(
      i64::parse("+42").or_panic_with_message("+42 is a number") == 42i64,
      "the sign is not a digit",
    )
  }

  test a_leading_minus_makes_the_value_negative() {
    assert(
      i64::parse("-42").or_panic_with_message("-42 is a number") == 0i64 - 42i64,
      "the sign applies to the magnitude",
    )
  }

  test zero_reads_as_zero() {
    assert(i64::parse("0").or_panic_with_message("0 is a number") == 0i64, "one zero digit")
    assert(
      i64::parse("-0").or_panic_with_message("-0 is a number") == 0i64,
      "a signed zero is still zero",
    )
  }

  test leading_zeros_do_not_change_the_value() {
    assert(
      i64::parse("007").or_panic_with_message("007 is a number") == 7i64,
      "the zeros add no magnitude",
    )
  }

  test an_empty_text_is_not_a_number() {
    assert(i64::parse("").is_none(), "there is no digit to read")
  }

  test a_bare_sign_is_not_a_number() {
    assert(i64::parse("+").is_none(), "a plus with no digits is not a number")
    assert(i64::parse("-").is_none(), "and neither is a minus")
  }

  test a_letter_among_the_digits_is_rejected() {
    assert(i64::parse("4a").is_none(), "the trailing letter is not a digit")
    assert(i64::parse("a4").is_none(), "and the leading one is not either")
  }

  test a_space_among_the_digits_is_rejected() {
    assert(i64::parse(" 42").is_none(), "a leading space is not a digit")
    assert(i64::parse("42 ").is_none(), "and neither is a trailing one")
  }

  test the_largest_i64_reads_back() {
    assert(
      i64::parse("9223372036854775807")
        .or_panic_with_message("the maximum is a number") ==
        9223372036854775807i64,
      "every digit of the maximum is read",
    )
  }

  test the_smallest_i64_reads_back() {
    assert(
      i64::parse("-9223372036854775808")
        .or_panic_with_message("the minimum is a number") ==
        0i64 -
        9223372036854775807i64 -
        1i64,
      "the magnitude of the minimum is one past the maximum",
    )
  }

  test one_past_the_largest_i64_is_rejected() {
    assert(
      i64::parse("9223372036854775808").is_none(),
      "the magnitude overflows, so no value is read",
    )
  }

  test one_below_the_smallest_i64_is_rejected() {
    assert(i64::parse("-9223372036854775809").is_none(), "the magnitude overflows by one")
  }

  test an_i32_reads_its_own_bounds() {
    assert(
      i32::parse("2147483647").or_panic_with_message("the maximum fits") == 2147483647i32,
      "the maximum",
    )
    assert(
      i32::parse("-2147483648")
        .or_panic_with_message("the minimum fits") ==
        0i32 -
        2147483647i32 -
        1i32,
      "the minimum",
    )
  }

  test an_i32_rejects_what_only_an_i64_holds() {
    assert(i32::parse("2147483648").is_none(), "one past the i32 maximum")
    assert(i32::parse("-2147483649").is_none(), "one below the i32 minimum")
  }

  test a_u64_rejects_a_sign_it_does_not_hold() {
    assert(u64::parse("-1").is_none(), "an unsigned reading has no negative half")
  }

  test a_u64_reads_its_own_maximum() {
    assert(
      u64::parse("18446744073709551615")
        .or_panic_with_message("the maximum fits") ==
        18446744073709551615u64,
      "every digit of the maximum is read",
    )
    assert(u64::parse("18446744073709551616").is_none(), "one past it overflows")
  }

  test a_u32_rejects_what_only_a_u64_holds() {
    assert(
      u32::parse("4294967295").or_panic_with_message("the maximum fits") == 4294967295u32,
      "the maximum",
    )
    assert(u32::parse("4294967296").is_none(), "one past it overflows")
  }

  test a_fraction_reads_its_text() {
    assert(
      f64::parse("1.5").or_panic_with_message("1.5 is a number") == 1.5f64,
      "the point separates the halves",
    )
  }

  test a_float_without_a_point_reads_as_a_whole_number() {
    assert(
      f64::parse("42").or_panic_with_message("42 is a number") == 42.0f64,
      "the fraction may be left out",
    )
  }

  test a_leading_point_reads_as_a_fraction() {
    assert(
      f64::parse(".5").or_panic_with_message(".5 is a number") == 0.5f64,
      "the digits before the point may be left out",
    )
  }

  test a_trailing_point_reads_as_a_whole_number() {
    assert(
      f64::parse("5.").or_panic_with_message("5. is a number") == 5.0f64,
      "the digits after the point may be left out",
    )
  }

  test a_second_point_is_rejected() {
    assert(f64::parse("1.2.3").is_none(), "a number carries one point")
  }

  test a_point_alone_is_not_a_number() {
    assert(f64::parse(".").is_none(), "there is no digit around the point")
  }

  test a_float_keeps_the_sign_of_a_zero() {
    zero := f64::parse("-0.0").or_panic_with_message("-0.0 is a number")
    assert(zero == 0.0f64, "the value is zero")
    assert(1.0f64 / zero < 0.0f64, "and the reciprocal is negative, so the sign survived")
  }

  test an_exponent_is_not_read() {
    assert(f64::parse("1e10").is_none(), "the exponent is not part of a decimal reading")
  }

  test digits_past_the_significand_round_once() {
    assert(
      f64::parse("0.9876543210987654321")
        .or_panic_with_message("the fraction is a number") ==
        0.9876543210987654321f64,
      "text and a literal of the same digits agree bit for bit",
    )
  }

  test a_long_fraction_reads_as_the_nearest_float() {
    assert(
      f64::parse("3.141592653589793238")
        .or_panic_with_message("the fraction is a number") ==
        3.141592653589793238f64,
      "the significand is assembled from the digits and rounded once, to nearest",
    )
  }

  test a_float_past_the_numerator_is_rejected() {
    assert(
      f64::parse("99999999999999999999").is_none(),
      "twenty digits pass what the numerator holds",
    )
  }

  test a_letter_among_float_digits_is_rejected() {
    assert(f64::parse("1.5x").is_none(), "the trailing letter is not a digit")
    assert(f64::parse("x1.5").is_none(), "and the leading one is not either")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
