// Indexing hands out a loan on the container, not on a place beside it,
// so a reference taken that way and held across a `push` that
// reallocates is the same conflict as one taken through an accessor.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(11i32)
  held := &v[0]
  v.push(22i32)
  ret *held - 11
}
