// A strided head yields every `stride`-th value starting from the
// start: `(0..<10).step_by(3)` is 0, 3, 6, 9. An inclusive end hit
// exactly is yielded without stepping past it; an overshoot stops
// before wrapping; inverted ranges stay empty; an open end runs
// until the body stops it.
fn main() -> i32 {
  mut excl := 0
  for i in (0..<10).step_by(3) {
    excl = excl + i
  }
  if excl != 18 { ret 1 }
  mut incl := 0
  for i in (0..=9).step_by(3) {
    incl = incl + i
  }
  if incl != 18 { ret 2 }
  // Signed values stride through zero.
  mut signed := 0
  for i in (-4..<5).step_by(3) {
    signed = signed + i
  }
  if signed != -3 { ret 3 }
  // Inverted ranges yield nothing at any stride.
  mut empty := 0
  for i in (5..<3).step_by(2) {
    empty = empty + 1
  }
  if empty != 0 { ret 4 }
  // An inclusive end at the width's maximum stops instead of
  // wrapping: 254 with stride 2 never computes 256.
  mut wide := 0i32
  for i in (250u8..=255u8).step_by(2) {
    wide = wide + (i as i32)
  }
  if wide != 756 { ret 5 }
  // An exclusive end stops before overshooting into a wrap: 253
  // with stride 3 never computes 256.
  mut narrow := 0i32
  for i in (250u8..<255u8).step_by(3) {
    narrow = narrow + (i as i32)
  }
  if narrow != 503 { ret 6 }
  // An open end strides until the body stops it.
  mut n := 0
  for i in (0..).step_by(4) {
    n = n + i
    if i >= 12 { break }
  }
  if n != 24 { ret 7 }
  // Stride one reads exactly like the plain cursor.
  mut one := 0
  for i in (0..<4).step_by(1) {
    one = one + i
  }
  if one != 6 { ret 8 }
  ret 0
}
