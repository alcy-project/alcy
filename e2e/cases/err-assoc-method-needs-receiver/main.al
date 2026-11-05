// A path names an associated function; a method takes its receiver
// from the value it is called on.

struct Counter {
  n: i32,
}

impl Counter {
  pub fn make(n: i32) -> Counter {
    ret Counter { n: n }
  }

  pub fn get(self: &Self) -> i32 {
    ret self.n
  }
}

fn main() -> i32 {
  c := Counter::make(3)
  ret Counter::get(&c)
}
