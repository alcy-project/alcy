// A tuple local that holds an owning value retires it.
fn main() -> i32 {
  t := (format("hi"), 1)
  if t.0.len() != 2 { ret 1 }
  ret 0
}
