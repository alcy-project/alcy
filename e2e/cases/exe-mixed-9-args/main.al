// Integer and float arguments that do not fit the register count spill
// into the outgoing area. The callee reads both kinds back in order.
fn take(a: i64, b: i64, c: i64, d: i64, e: f64, f: f64, g: f64, h: f64, i: f64) -> i32 {
  if a != 1 || b != 2 || c != 3 || d != 4 {
    ret 1
  }
  if e != 5.0f64 || f != 6.0f64 || g != 7.0f64 || h != 8.0f64 {
    ret 2
  }
  if i != 9.0f64 {
    ret 3
  }
  ret 0
}

fn main() -> i32 {
  ret take(1, 2, 3, 4, 5.0, 6.0, 7.0, 8.0, 9.0)
}
