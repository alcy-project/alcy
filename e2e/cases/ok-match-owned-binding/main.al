// An arm binding of an owned payload ends with the arm. Its slot lives
// in the arm's block, so a scope-exit drop in the block after the join
// did not dominate it and the emitted IR was invalid.
fn f(o: Option<String>) -> i32 {
  r := match o {
    Option::Some(s) => s.len() as i32,
    Option::None => 0,
  }
  ret r
}

fn main() -> i32 {
  ret f(Option::None)
}
