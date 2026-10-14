// A reference to a block-local may not be the block's value.
fn main() -> i32 {
  r := {
    x := 5
    &x
  }
  ret *r
}
