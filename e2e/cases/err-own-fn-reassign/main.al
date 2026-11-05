// The slot holds the witness the caller instantiated, so a second
// value cannot take its place (ADR-0075).
fn twice(x: i32) -> i32 {
  ret x + x
}

fn apply(mut f: fn (i32) -> i32) -> i32 {
  f = twice
  ret f(2)
}

fn main() -> i32 {
  ret apply(twice)
}
