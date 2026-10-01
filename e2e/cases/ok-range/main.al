// Ranges are values and runs narrow views: every spelling checks,
// and a run of a fixed array is named through a borrow.
fn main() -> i32 {
  mut a := [10i32, 20i32, 30i32, 40i32, 50i32]
  r := 1..<3
  v := &a[r]
  w := &a[..]
  x := w[1..=3]
  y := x[..<1]
  n := v[0] + y[0] + slice_len(w) as i32
  mut m := &mut a[0..<2]
  m[0] = 99i32
  ret n + m[0]
}
