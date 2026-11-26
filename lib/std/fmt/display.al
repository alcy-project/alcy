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

#[when(testing)] {
  #[test] fn rendering_is_the_bytes_of_the_string() {
    hello := String::from("hello")
    mut out := String::from("........")
    n := hello.display(unsafe { out.as_mut_slice() })
    assert(n == 5, "the count is the length of the rendering")
    assert(
      out.byte_slice(0, 5).or_panic_with_message("the slice lands at the front") == "hello",
      "the bytes land at the front",
    )
    assert(out.byte(5) == 0x2eu8, "the rest of the slice is untouched")
  }

  #[test] fn empty_string_renders_as_no_bytes() {
    empty := String::from("")
    mut out := String::from("..")
    n := empty.display(unsafe { out.as_mut_slice() })
    assert(n == 0, "there is nothing to render")
    assert(out.byte(0) == 0x2eu8, "the slice is untouched")
  }

  #[test] fn empty_slice_is_the_measurement_pass() {
    hello := String::from("hello")
    mut nothing := String::with_capacity(0)
    n := hello.display(unsafe { nothing.as_mut_slice() })
    assert(n == 5, "the count comes back with no room to write into")
  }

  #[test] fn short_slice_takes_the_prefix_and_the_count_still_names_the_whole() {
    hello := String::from("hello")
    mut out := String::from("...")
    n := hello.display(unsafe { out.as_mut_slice() })
    assert(n == 5, "the count is not truncated")
    assert(
      out.byte_slice(0, 3).or_panic_with_message("the slice takes what fits") == "hel",
      "the slice takes what fits",
    )
  }

  #[test] fn zero_byte_is_a_byte_of_the_rendering() {
    bytes := String::from("a\u{0000}b")
    mut out := String::from("...")
    n := bytes.display(unsafe { out.as_mut_slice() })
    assert(n == 3, "the rendering is three bytes")
    assert(out.byte(1) == 0u8, "the zero byte is copied like any other")
    assert(out.byte(2) == 0x62u8, "the byte after it follows")
  }

  #[test] fn multibyte_character_counts_as_its_bytes() {
    e_acute := String::from("\u{00e9}")
    mut out := String::from("..")
    n := e_acute.display(unsafe { out.as_mut_slice() })
    assert(n == 2, "one character, two bytes")
    assert(out.byte(0) == 0xc3u8, "the first byte of the encoding lands")
    assert(out.byte(1) == 0xa9u8, "the second byte of the encoding lands")
  }

  #[test] fn measure_pass_then_a_write_pass_fills_the_buffer_it_sized() {
    text := String::from("hello")
    mut nothing := String::with_capacity(0)
    n := text.display(unsafe { nothing.as_mut_slice() })
    mut out := String::with_capacity(n)
    unsafe { out.set_len(n) }
    written := text.display(unsafe { out.as_mut_slice() })
    assert(written == n, "the writing pass agrees with the measuring pass")
    assert(out.as_str() == "hello", "the buffer holds the rendering")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}

// Primitive conversions live here as free functions rather than spec
// methods: method lookup resolves `Display` on nominal types only.
// The destination is a raw pointer and a capacity so the expansion
// never lends an owned temporary: a mutable borrow would make drop
// elaboration end that temporary early.
pub fn display_str(value: str, out: *mut u8, cap: usize) -> usize {
  n := str_len(value)
  if cap < n {
    ret n
  }
  mut i := 0 as usize
  while i < n {
    unsafe { *ptr_offset_mut(out, i as isize) = str_byte(value, i) }
    i += 1
  }
  ret n
}

// A char is already an ASCII byte, so it writes as one byte.
pub fn display_char(value: char, out: *mut u8, cap: usize) -> usize {
  if cap == 0 {
    ret 1
  }
  unsafe { *ptr_offset_mut(out, 0 as isize) = value as u8 }
  ret 1
}

// A rune writes as the UTF-8 bytes of its scalar, the same spelling a
// `str` holding that scalar would have.
pub fn display_rune(value: rune, out: *mut u8, cap: usize) -> usize {
  code := value as u32
  mut buf := [0u8, 0u8, 0u8, 0u8]
  mut n := 0 as usize
  if code <= 0x7Fu32 {
    buf[0] = code as u8
    n = 1
  } else if code <= 0x7FFu32 {
    buf[0] = (0xC0u32 | (code >> 6)) as u8
    buf[1] = (0x80u32 | (code & 0x3Fu32)) as u8
    n = 2
  } else if code <= 0xFFFFu32 {
    buf[0] = (0xE0u32 | (code >> 12)) as u8
    buf[1] = (0x80u32 | ((code >> 6) & 0x3Fu32)) as u8
    buf[2] = (0x80u32 | (code & 0x3Fu32)) as u8
    n = 3
  } else {
    buf[0] = (0xF0u32 | (code >> 18)) as u8
    buf[1] = (0x80u32 | ((code >> 12) & 0x3Fu32)) as u8
    buf[2] = (0x80u32 | ((code >> 6) & 0x3Fu32)) as u8
    buf[3] = (0x80u32 | (code & 0x3Fu32)) as u8
    n = 4
  }
  if cap >= n {
    mut i := 0 as usize
    while i < n {
      unsafe { *ptr_offset_mut(out, i as isize) = buf[i] }
      i += 1
    }
  }
  ret n
}

pub fn display_bool(value: bool, out: *mut u8, cap: usize) -> usize {
  yes := [0x74u8, 0x72u8, 0x75u8, 0x65u8]
  no := [0x66u8, 0x61u8, 0x6cu8, 0x73u8, 0x65u8]
  mut bytes := no.as_slice()
  if value {
    bytes = yes.as_slice()
  }
  n := bytes.len()
  if cap < n {
    ret n
  }
  mut i := 0 as usize
  while i < n {
    unsafe { *ptr_offset_mut(out, i as isize) = bytes[i] }
    i += 1
  }
  ret n
}

pub fn display_u64(value: u64, out: *mut u8, cap: usize) -> usize {
  mut scratch := [0u8; 20]
  mut n := value
  mut count := 0 as usize
  loop {
    digit := n % 10u64
    n = n / 10u64
    scratch[19 - count] = (digit as u8) + 48u8
    count += 1
    if n == 0u64 {
      break
    }
  }
  if cap < count {
    ret count
  }
  mut i := 0 as usize
  while i < count {
    unsafe { *ptr_offset_mut(out, i as isize) = scratch[20 - count + i] }
    i += 1
  }
  ret count
}

fn put_byte(out: *mut u8, at: usize, b: u8, cap: usize) {
  if at < cap {
    unsafe { *ptr_offset_mut(out, at as isize) = b }
  }
}

fn digits_u64(value: u64, out: *mut u8, cap: usize) -> usize {
  mut scratch := [0u8; 20]
  mut n := value
  mut count := 0 as usize
  loop {
    d := n % 10u64
    n = n / 10u64
    scratch[19 - count] = (d as u8) + 48u8
    count += 1
    if n == 0u64 {
      break
    }
  }
  mut i := 0 as usize
  while i < count && i < cap {
    unsafe { *ptr_offset_mut(out, i as isize) = scratch[20 - count + i] }
    i += 1
  }
  ret count
}

struct FixedResult {
  written: usize,
  carry: bool,
}

fn float_fixed(v: f64, digits: usize, trim: bool, out: *mut u8, cap: usize, fold: bool) -> FixedResult {
  whole := v as u64
  diff := v - (whole as f64)
  integral := v >= 9007199254740992.0f64
  mut fraction := diff
  if integral {
    fraction = 0.0f64
  }
  mut dyadic := fraction
  mut power := 0u64
  loop {
    t := dyadic as u64
    if (t as f64) == dyadic {
      break
    }
    dyadic = dyadic * 2.0f64
    power += 1
  }
  numerator := dyadic as u64
  mut constant := 1u64
  mut ci := 0 as usize
  while ci < digits {
    constant = constant * 10u64
    ci += 1
  }
  m_low := numerator & 0xffffffffu64
  m_high := numerator >> 32
  p0 := m_low * (constant & 0xffffffffu64)
  p1 := m_low * (constant >> 32)
  p2 := m_high * (constant & 0xffffffffu64)
  p3 := m_high * (constant >> 32)
  middle := (p0 >> 32) + (p1 & 0xffffffffu64) + (p2 & 0xffffffffu64)
  product_low := (p0 & 0xffffffffu64) | (middle << 32)
  product_high := p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32)
  split := wrapping_sub(power, 1u64)
  low_window := split < 64u64
  high_window := split >= 64u64
  past_windows := split >= 128u64
  mut low_count := 0u64
  if low_window {
    low_count = split
  }
  cross_raw := wrapping_sub(63u64, split)
  mut cross_count := 0u64
  if low_window {
    cross_count = cross_raw
  }
  above_raw := wrapping_sub(split, 64u64)
  mut above_count := 0u64
  if high_window {
    above_count = above_raw
  }
  mut high_count := above_count
  if past_windows {
    high_count = 0u64
  }
  crossed := product_high << cross_count
  crossed_again := crossed << 1
  low_digits := product_low >> low_count
  below_path := crossed_again | low_digits
  above_path := product_high >> high_count
  middle_path := high_window && !past_windows
  mut selected := 0u64
  if middle_path {
    selected = above_path
  }
  mut quotient := 0u64
  if low_window {
    quotient = below_path
  } else {
    quotient = selected
  }
  mut exact := quotient >> 1
  round_bit := quotient & 1u64
  mut buf := [48u8; 18]
  mut i := digits
  while i > 0 {
    i -= 1
    buf[i] = ((exact % 10u64) as u8) + 48u8
    exact = exact / 10u64
  }
  mut carry := round_bit != 0u64
  i = digits
  while i > 0 {
    i -= 1
    summed :=
      (buf[i] - 48u8) +
      (
        if carry {
          1u8
        } else {
          0u8
        }
      )
    if summed == 10u8 {
      buf[i] = 48u8
      carry = true
    } else {
      buf[i] = summed + 48u8
      carry = false
    }
  }
  adjusted :=
    whole +
    (
      if carry {
        1u64
      } else {
        0u64
      }
    )
  mut printed := adjusted
  mut carry_out := carry
  if fold {
    if adjusted == 10u64 {
      printed = 1u64
      carry_out = true
    } else {
      carry_out = false
    }
  }
  mut n := digits_u64(printed, out, cap)
  mut length := digits
  if trim {
    while length > 0 && buf[length - 1] == 48u8 {
      length -= 1
    }
  }
  if length > 0 {
    put_byte(out, n, 46u8, cap)
    n += 1
  }
  mut j := 0 as usize
  while j < length {
    put_byte(out, n, buf[j], cap)
    n += 1
    j += 1
  }
  ret FixedResult { written: n, carry: carry_out }
}

fn decimal_scale(places: i32) -> f64 {
  highs :=
    [
      1e0f64,
      1e16f64,
      1e32f64,
      1e48f64,
      1e64f64,
      1e80f64,
      1e96f64,
      1e112f64,
      1e128f64,
      1e144f64,
      1e160f64,
      1e176f64,
      1e192f64,
      1e208f64,
      1e224f64,
      1e240f64,
      1e256f64,
      1e272f64,
      1e288f64,
      1e304f64,
      1.0f64 / 0.0f64,
    ]
  lows :=
    [
      1e0f64,
      1e1f64,
      1e2f64,
      1e3f64,
      1e4f64,
      1e5f64,
      1e6f64,
      1e7f64,
      1e8f64,
      1e9f64,
      1e10f64,
      1e11f64,
      1e12f64,
      1e13f64,
      1e14f64,
      1e15f64,
    ]
  chunk := places / 16
  rest := places - chunk * 16
  ret highs[chunk as usize] * lows[rest as usize]
}

fn exp_digits(magnitude: f64, big: bool, digits: usize, trim: bool, out: *mut u8, cap: usize) -> usize {
  mut m := magnitude
  mut e := 0i32
  loop {
    mut again := false
    if big {
      if m >= 10.0f64 {
        again = true
      }
    } else {
      if m < 1.0f64 {
        again = true
      }
    }
    if !again {
      break
    }
    if big {
      m = m * 0.1f64
      e += 1
    } else {
      m = m * 10.0f64
      e -= 1
    }
  }
  mut places := e
  if e < 0i32 {
    places = 0i32 - e
  }
  whole := decimal_scale(places)
  overflowed := whole > 1.7976931348623157e308f64
  direct_tiny := magnitude * whole
  lower_places := places / 2
  upper_places := places - lower_places
  partial := magnitude * decimal_scale(lower_places)
  staged := partial * decimal_scale(upper_places)
  mut tiny := staged
  if !overflowed {
    tiny = direct_tiny
  }
  divided := magnitude / whole
  mut mantissa := tiny
  if big {
    mantissa = divided
  }
  too_big := mantissa >= 10.0f64
  too_small := mantissa < 1.0f64
  mut picked := mantissa
  if too_small {
    picked = mantissa * 10.0f64
  }
  mut adjusted_m := picked
  if too_big {
    adjusted_m = mantissa * 0.1f64
  }
  mut base_e := e
  if too_big {
    base_e += 1
  }
  if too_small {
    base_e -= 1
  }
  written := float_fixed(adjusted_m, digits, trim, out, cap, true)
  mut exponent := base_e
  if written.carry {
    exponent += 1
  }
  put_byte(out, written.written, 101u8, cap)
  mut n := written.written + 1
  negative := exponent < 0i32
  if negative {
    put_byte(out, n, 45u8, cap)
    n += 1
  }
  mut magnitude_e := exponent
  if negative {
    magnitude_e = 0i32 - exponent
  }
  mut rest := 0 as usize
  if cap > n {
    rest = cap - n
  }
  n += digits_u64(magnitude_e as u64, unsafe { ptr_offset_mut(out, n as isize) }, rest)
  ret n
}

fn write_bytes(text: str, out: *mut u8, cap: usize) -> usize {
  n := str_len(text)
  mut i := 0 as usize
  while i < n && i < cap {
    unsafe { *ptr_offset_mut(out, i as isize) = str_byte(text, i) }
    i += 1
  }
  ret n
}

pub fn display_f64(value: f64, precision: usize, trim: bool, out: *mut u8, cap: usize) -> usize {
  if value != value {
    ret write_bytes("NaN", out, cap)
  }
  reciprocal := 1.0f64 / value
  negative := value < 0.0f64 || reciprocal < 0.0f64
  mut n := 0 as usize
  if negative {
    put_byte(out, 0, 45u8, cap)
    n = 1
  }
  mut remaining := 0 as usize
  if cap > n {
    remaining = cap - n
  }
  if reciprocal == 0.0f64 {
    m := write_bytes("inf", unsafe { ptr_offset_mut(out, n as isize) }, remaining)
    ret n + m
  }
  mut magnitude := value
  if negative {
    magnitude = value * -1.0f64
  }
  mut digits := 6 as usize
  if !trim {
    digits = precision
  }
  big := magnitude >= 1e16f64
  scientific := big || (magnitude > 0.0f64 && magnitude < 1e-4f64)
  mut written := 0 as usize
  if scientific {
    written =
      exp_digits(
        magnitude,
        big,
        digits,
        trim,
        unsafe { ptr_offset_mut(out, n as isize) },
        remaining,
      )
  } else {
    written =
      float_fixed(
        magnitude,
        digits,
        trim,
        unsafe { ptr_offset_mut(out, n as isize) },
        remaining,
        false,
      )
      .written
  }
  ret n + written
}

fn write_signed(value: i64, out: *mut u8, cap: usize) -> usize {
  negative := value < 0i64
  mut n := 0 as usize
  if negative {
    if cap > 0 {
      unsafe { *ptr_offset_mut(out, 0) = 45u8 }
    }
    n = 1
  }
  mut magnitude := value as u64
  if negative {
    magnitude = wrapping_sub(0u64, value as u64)
  }
  mut rest := 0 as usize
  if cap > n {
    rest = cap - n
  }
  written := display_u64(magnitude, unsafe { ptr_offset_mut(out, n as isize) }, rest)
  ret n + written
}

impl Display for i8 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := write_signed(*self as i64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for i16 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := write_signed(*self as i64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for i32 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := write_signed(*self as i64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for i64 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := write_signed(*self as i64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for isize {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := write_signed(*self as i64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for u8 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_u64(*self as u64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for u16 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_u64(*self as u64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for u32 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_u64(*self as u64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for u64 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_u64(*self as u64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for usize {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_u64(*self as u64, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for bool {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_bool(*self, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for str {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    text := *self
    n := str_len(text)
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = str_byte(text, i)
      i += 1
    }
    ret n
  }
}

impl Display for f32 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_f64(*self as f64, 6, true, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

impl Display for f64 {
  fn display(self: &Self, out: &mut [u8]) -> usize {
    mut scratch := [0u8; 64]
    n := display_f64(*self, 6, true, &mut scratch[0] as *mut u8, scratch.len())
    mut i := 0 as usize
    while i < n && i < out.len() {
      out[i] = scratch[i]
      i += 1
    }
    ret n
  }
}

// The compile-time view of a format string. `comp_pieces` decodes the
// literal into these pieces so an Alcy body can decide what each
// placeholder emits; the value exists only during compilation.
pub struct Piece {
  pub is_arg: bool,
  pub literal: str,
  pub precision: usize,
  pub arg: usize,
  pub explicit_float: bool,
}

pub intrinsic fn comp_pieces(fmt: str) -> [Piece; 0];

// A float placeholder with an explicit precision takes this path; the
// plain `Display` rendering keeps its fixed six digits with trimming.
pub spec DisplayPrecision {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize;
}

impl DisplayPrecision for f32 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_f64(*self as f64, precision, trim, &mut scratch[0] as *mut u8, 0)
    }
    ret display_f64(*self as f64, precision, trim, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for f64 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_f64(*self as f64, precision, trim, &mut scratch[0] as *mut u8, 0)
    }
    ret display_f64(*self, precision, trim, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for i8 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret write_signed(*self as i64, &mut scratch[0] as *mut u8, 0)
    }
    ret write_signed(*self as i64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for i16 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret write_signed(*self as i64, &mut scratch[0] as *mut u8, 0)
    }
    ret write_signed(*self as i64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for i32 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret write_signed(*self as i64, &mut scratch[0] as *mut u8, 0)
    }
    ret write_signed(*self as i64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for i64 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret write_signed(*self as i64, &mut scratch[0] as *mut u8, 0)
    }
    ret write_signed(*self as i64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for isize {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret write_signed(*self as i64, &mut scratch[0] as *mut u8, 0)
    }
    ret write_signed(*self as i64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for u8 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_u64(*self as u64, &mut scratch[0] as *mut u8, 0)
    }
    ret display_u64(*self as u64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for u16 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_u64(*self as u64, &mut scratch[0] as *mut u8, 0)
    }
    ret display_u64(*self as u64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for u32 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_u64(*self as u64, &mut scratch[0] as *mut u8, 0)
    }
    ret display_u64(*self as u64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for u64 {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_u64(*self as u64, &mut scratch[0] as *mut u8, 0)
    }
    ret display_u64(*self as u64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for usize {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_u64(*self as u64, &mut scratch[0] as *mut u8, 0)
    }
    ret display_u64(*self as u64, &mut out[0] as *mut u8, out.len())
  }
}

impl DisplayPrecision for bool {
  fn display_precision(self: &Self, precision: usize, trim: bool, out: &mut [u8]) -> usize {
    if out.len() == 0 {
      mut scratch := [0u8; 1]
      ret display_bool(*self, &mut scratch[0] as *mut u8, 0)
    }
    ret display_bool(*self, &mut out[0] as *mut u8, out.len())
  }
}
