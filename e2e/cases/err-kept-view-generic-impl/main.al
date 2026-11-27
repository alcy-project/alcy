// A generic implementation's keep reaches a dispatch through the bound:
// the body's own item answers for the implementation the spec
// declaration names.

spec Sink {
  fn put(mut self: &mut Self, s: str);
}

struct Box2<T> {
  v: Vec<str>,
  t: T,
}

impl<T> Sink for Box2<T> {
  fn put(mut self: &mut Self, s: str) {
    self.v.push(s)
  }
}

fn store<T: Sink>(mut v: &mut T, s: str) {
  v.put(s)
}

fn main() -> i32 {
  mut b: Box2<i32> := Box2 { v: Vec::<str>::new(), t: 0 }
  {
    x := String::from("abc")
    store(&mut b, x.as_str())
  }
  w := b.v.at(0).or_panic("none")
  print("[{}]", *w)
  ret 0
}
