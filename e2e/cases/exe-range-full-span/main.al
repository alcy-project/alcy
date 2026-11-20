// A range spans the whole element type: an inclusive end at the
// width's maximum is reached without computing one past it, and the
// endpoint arithmetic stays inside the type even when the distance
// between the endpoints does not fit in it.
fn main() -> i32 {
  // The signed full span, inclusive: 256 values, whose total is -128.
  mut count := 0
  mut total := 0
  for i in -128i8..=127i8 {
    count = count + 1
    total = total + (i as i32)
  }
  if count != 256 {
    ret 1
  }
  if total != -128 {
    ret 2
  }
  // The same span with an exclusive end: 255 values ending at 126.
  mut excl := 0
  mut excl_total := 0
  for i in -128i8..<127i8 {
    excl = excl + 1
    excl_total = excl_total + (i as i32)
  }
  if excl != 255 {
    ret 3
  }
  if excl_total != -255 {
    ret 4
  }
  // The unsigned full span still stops at the maximum.
  mut uns := 0
  mut uns_total := 0
  for i in 0u8..=255u8 {
    uns = uns + 1
    uns_total = uns_total + (i as i32)
  }
  if uns != 256 {
    ret 5
  }
  if uns_total != 32640 {
    ret 6
  }
  // Short spans at both ends of the signed type.
  mut low := 0
  for i in -128i8..=-121i8 {
    low = low + 1
  }
  if low != 8 {
    ret 7
  }
  mut high := 0
  for i in 120i8..=127i8 {
    high = high + 1
  }
  if high != 8 {
    ret 8
  }
  // A stride wider than the distance an endpoint subtraction could
  // hold: three values, from the minimum through the maximum.
  mut wide := 0
  mut wide_total := 0i64
  for i in (-2147483648i32..=2147483647i32).step_by(2147483647) {
    wide = wide + 1
    wide_total = wide_total + (i as i64)
  }
  if wide != 3 {
    ret 9
  }
  if wide_total != -3i64 {
    ret 10
  }
  // The same shape at 64 bits, where the distance needs 65.
  mut long := 0
  mut first := 0i64
  mut last := 0i64
  for i in (-9223372036854775807i64..=9223372036854775807i64).step_by(9223372036854775807) {
    if long == 0 {
      first = i
    }
    last = i
    long = long + 1
  }
  if long != 3 {
    ret 11
  }
  if first != -9223372036854775807i64 {
    ret 12
  }
  if last != 9223372036854775807i64 {
    ret 13
  }
  // A hand-built interval may start excluded, and a start at the
  // width's maximum wraps to the minimum rather than leaving the type:
  // past 255 is 0, which reaches 1.
  mut wrap := 0
  mut wrap_total := 0
  built: Range<u8> := Range { start: Bound::Excluded(255u8), end: Bound::Included(1u8) }
  for i in built {
    wrap = wrap + 1
    wrap_total = wrap_total + (i as i32)
  }
  if wrap != 2 {
    ret 14
  }
  if wrap_total != 1 {
    ret 15
  }
  ret 0
}
