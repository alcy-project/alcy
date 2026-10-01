// A range that names an end must say whether the end is included:
// `..<` excludes and `..=` includes. Bare `..` is the unbounded
// spelling, so it cannot stand in for either.
fn main() -> i32 {
  r := 1..3
  ret 0
}
