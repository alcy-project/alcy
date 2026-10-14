// A tuple returned from a call and bound retires its owning field.
fn make() -> (String, i32) {
  ret (format("hi"), 1)
}

fn main() -> i32 {
  t := make()
  if t.0.len() != 2 { ret 1 }
  if t.1 != 1 { ret 2 }
  ret 0
}
