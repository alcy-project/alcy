// A unit variant infers its instance from the binding annotation or the
// return type, so `Option::None` stands without a turbofish.
fn none() -> Option<i32> {
  ret Option::None
}

fn main() -> i32 {
  n: Option<i32> := Option::None
  if n.is_some() { ret 1 }
  if none().is_some() { ret 2 }
  ret 0
}
