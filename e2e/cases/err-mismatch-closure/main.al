// A closure mismatch spells the two signatures.
fn apply(f: (i32) -> i32) -> i32 { ret f(1) }

fn main() -> i32 {
  ret apply((x: i64) -> 1)
}
