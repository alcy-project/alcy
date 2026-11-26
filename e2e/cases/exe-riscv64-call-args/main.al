// A ninth argument no longer fits a register. The caller spills it into
// the frame's outgoing area and the callee reads it back from there.
fn take(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64, g: i64, h: i64, i: i64) -> i32 {
  ret i as i32
}

fn main() -> i32 {
  ret take(1, 2, 3, 4, 5, 6, 7, 8, 9)
}
