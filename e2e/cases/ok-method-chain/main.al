// A method that calls another method on its own receiver.
struct Counter {
  n: i32,
}

impl Counter {
  fn new() -> Counter {
    ret Counter { n: 0 }
  }

  fn add(mut self: &mut Self, k: i32) {
    self.n = self.n + k
  }

  fn add_twice(mut self: &mut Self, k: i32) {
    self.add(k)
    self.add(k)
  }
}

fn main() -> i32 {
  mut c := Counter::new()
  c.add_twice(3)
  if c.n != 6 {
    ret 1
  }
  ret 0
}
