// The generic implementation only reads the view, so the same generic
// helper accepts the call.

spec Writer {
  fn write(mut self: &mut Self, s: str);
}

struct Noop2<T> {
  n: i32,
  t: T,
}

impl<T> Writer for Noop2<T> {
  fn write(mut self: &mut Self, s: str) {
    self.n = self.n + (s.len_bytes() as i32)
  }
}

fn use_it<T: Writer>(mut v: &mut T, s: str) {
  v.write(s)
}

fn main() -> i32 {
  mut n: Noop2<i32> := Noop2 { n: 0, t: 0 }
  s := String::from("abc")
  use_it(&mut n, s.as_str())
  ret n.n - 3
}
