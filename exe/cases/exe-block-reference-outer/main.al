// A reference to an outer local outlives the block, so it is fine.
fn main() -> i32 {
  x := 5
  r := {
    &x
  }
  ret *r
}
