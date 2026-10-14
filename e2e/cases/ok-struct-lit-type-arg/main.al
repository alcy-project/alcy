// A struct literal's turbofish carries the instantiation.
struct Holder<V> {
  v: V,
}
impl<V> Holder<V> {
  fn get(self: &Self) -> &V {
    ret &self.v
  }
}
fn main() -> i32 {
  h := Holder::<i32> { v: 7 }
  if *h.get() != 7 { ret 1 }
  p := Holder::<String> { v: format("x") }
  if p.get().as_str() != "x" { ret 2 }
  ret 0
}
