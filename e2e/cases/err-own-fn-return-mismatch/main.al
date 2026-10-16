// The value a return names must carry the signature the spelling
// declares.
fn bad() -> fn (i32) -> i32 {
  ret 5
}

fn main() -> i32 {
  f := bad()
  ret f(1)
}
