// Reading a non-`Copy` value out of a reference as a value: the read
// would take what the reference's owner still ends.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(7)
  r := &v
  w := *r
  ret w.len() as i32
}
