// A generic body dispatches through a spec bound, so its keep summary
// has to answer for the implementation: the view stored by the impl is
// refused at the call through the generic helper.

spec Sink {
  fn put(mut self: &mut Self, s: str);
}

struct Buf {
  v: Vec<str>,
}

impl Sink for Buf {
  fn put(mut self: &mut Self, s: str) {
    self.v.push(s)
  }
}

fn store<T: Sink>(mut v: &mut T, s: str) {
  v.put(s)
}

fn main() -> i32 {
  mut b := Buf { v: Vec::<str>::new() }
  {
    x := String::from("abc")
    store(&mut b, x.as_str())
  }
  w := b.v.at(0).or_panic_with_message("none")
  print("[{}]", *w)
  ret 0
}
