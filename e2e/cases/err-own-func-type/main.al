// The return position waits for its slice: the synthetic parameter a
// parameter spelling creates is inferred from the argument, and a
// return has no argument to infer from yet.
fn make() -> fn(i32) -> i32 {
  t := 21
  ret [t] (a: i32) -> a + t
}

fn main() -> i32 {
  f := make()
  ret f(1) - 22
}
