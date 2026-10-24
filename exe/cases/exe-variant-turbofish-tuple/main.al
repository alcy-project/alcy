// A tuple variant names its instance with a turbofish and carries its
// payload.
fn main() -> i32 {
  got := Option::<i32>::Some(3i32)
  if got.or_panic_with_message("expected a value") != 3 {
    ret 1
  }
  s := Option::<str>::Some("x")
  match s {
    Option::Some(v) => {
      if v != "x" {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  ret 0
}
