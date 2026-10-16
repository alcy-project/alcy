// A guarded arm can fail, so it does not make the match exhaustive.

fn main() -> i32 {
  ret match 1 {
    x if x > 0 => 0,
  }
}
