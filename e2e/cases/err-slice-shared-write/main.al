// A `&[T]` is a read-only view: naming the binding `mut` lets it
// change, not the elements behind it. Writing through a shared
// reference is rejected, exactly as for any other `&T`.
fn main() -> i32 {
  mut v := Vec::<i32>::new()
  v.push(1i32)
  mut s := v.as_slice()
  s[0] = 7
  ret s[0] - 7
}
