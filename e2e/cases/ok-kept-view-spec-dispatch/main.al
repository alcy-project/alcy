// An implementation that only reads the view does not keep it, so the
// same generic helper accepts the call.

spec Writer {
  fn write(mut self: &mut Self, s: str);
}

struct Noop {
  n: i32,
}

impl Writer for Noop {
  fn write(mut self: &mut Self, s: str) {
    self.n = self.n + (s.len_bytes() as i32)
  }
}

fn use_it<T: Writer>(mut v: &mut T, s: str) {
  v.write(s)
}

fn main() -> i32 {
  mut n := Noop { n: 0 }
  s := String::from("abc")
  use_it(&mut n, s.as_str())
  ret n.n - 3
}
