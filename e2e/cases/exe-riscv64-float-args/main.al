// A ninth floating-point argument spills to the outgoing area.
fn take(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, g: f64, h: f64, i: f64) -> i32 {
  if i != 9.0f64 {
    ret 1
  }
  ret 0
}

fn main() -> i32 {
  ret take(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0)
}
