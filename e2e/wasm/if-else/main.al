// Both arms of a conditional, with a subtraction that would go negative
// before the branch.
fn abs(x: i32) -> i32 {
  if x < 0 {
    ret 0 - x
  }
  ret x
}

fn main() -> i32 {
  ret abs(-9) - 9
}
