// Two closure literals share one signature and still name two nominals,
// so a value of one is not a value of the other, and the diagnostic says
// which literal each side is.
fn keeper() -> fn () -> i32 {
  n := 41
  ret [&n]() -> n + 1
}

fn plain() -> fn () -> i32 {
  m := 41
  ret [m]() -> m + 1
}

fn main() -> i32 {
  mut f := keeper()
  f = plain()
  println("{}", f())
  ret 0
}
