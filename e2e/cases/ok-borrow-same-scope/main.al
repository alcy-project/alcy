// A borrow stored into a binding declared in the same block is fine:
// both slots end together, so the reference never reads a slot the
// block already ended. The inner binding here lives as long as the
// reference that names it.
fn main() -> i32 {
  mut out := 0
  {
    n := 5
    mut r: &i32 := &0
    r = &n
    out = *r
  }
  ret out
}
