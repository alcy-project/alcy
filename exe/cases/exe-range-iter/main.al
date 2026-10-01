// A range iterates through its cursor: exclusive and inclusive
// ends, empty and inverted ranges, an open end stopped by the body,
// and a cursor that stays spent. The head is `Copy`, so naming it
// twice iterates twice.
fn main() -> i32 {
  mut sum := 0
  for i in 0..<5 {
    sum = sum + i
  }
  if sum != 10 { ret 1 }
  mut inc := 0
  for i in 0..=5 {
    inc = inc + i
  }
  if inc != 15 { ret 2 }
  // Signed values ascend through zero.
  mut signed := 0
  for i in -3..<2 {
    signed = signed + i
  }
  if signed != -5 { ret 3 }
  // Inverted and empty ranges yield nothing.
  mut empty := 0
  for i in 5..<3 {
    empty = empty + 1
  }
  if empty != 0 { ret 4 }
  mut empty_inc := 0
  for i in 5..=3 {
    empty_inc = empty_inc + 1
  }
  if empty_inc != 0 { ret 5 }
  mut one := 0
  for i in 3..=3 {
    one = one + i
  }
  if one != 3 { ret 6 }
  // An inclusive end at the width's maximum never steps past it:
  // yielding 255 must not compute 255 + 1.
  mut wide := 0i32
  for i in 250u8..=255u8 {
    wide = wide + (i as i32)
  }
  if wide != 1515 { ret 7 }
  // An open end yields until the body stops it.
  mut n := 0
  for i in 0.. {
    n = n + i
    if i >= 10 { break }
  }
  if n != 55 { ret 8 }
  // A spent cursor stays spent.
  r := 1..<4
  mut it := r.into_iter()
  mut k := 0
  loop {
    match it.next() {
      Option::Some(v) => {
        k = k + v
      }
      Option::None => break,
    }
  }
  if k != 6 { ret 9 }
  if it.next().is_some() { ret 10 }
  mut again := 0
  for i in r {
    again = again + i
  }
  if again != 6 { ret 11 }
  ret 0
}
