// Two specs declare a method with the same name, and only one generic
// implementation keeps the view. The keep answers for the
// implementation the dispatch names, not for every same-named method.

spec A {
  fn put(mut self: &mut Self, s: str);
}

spec B {
  fn put(mut self: &mut Self, s: str);
}

struct KaG<T> {
  v: Vec<str>,
  t: T,
}

struct KbG<T> {
  n: i32,
  t: T,
}

impl<T> A for KaG<T> {
  fn put(mut self: &mut Self, s: str) {
    self.v.push(s)
  }
}

impl<T> B for KbG<T> {
  fn put(mut self: &mut Self, s: str) {
    self.n = self.n + (s.len_bytes() as i32)
  }
}

fn use_b<T: B>(mut v: &mut T, s: str) {
  v.put(s)
}

fn main() -> i32 {
  mut k: KbG<i32> := KbG { n: 0, t: 0 }
  s := String::from("abc")
  use_b(&mut k, s.as_str())
  ret k.n - 3
}
