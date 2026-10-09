// A `usize` return takes its literal's type from the checker, not from
// lowering's own default: `ret 2` here is a u64/i64 literal, and an i32
// one made the emitted IR invalid.
fn two() -> usize {
  ret 2
}

fn main() -> i32 {
  ret (two() as i32) - 2
}
