// A specification's method may declare type parameters of its own, and a
// concrete record reaches it: the arguments the declaration's bounds are
// compared with are the implementation's, which are the specification's,
// not the record's, which carries none.
spec Named {
  fn name(self: &Self) -> i32;
}

spec Mapper<T: Named> {
  fn map<U>(self: &Self, other: &T, fill: &U) -> i32;
}

struct Source {
  base: i32,
}

struct Target {
  base: i32,
}

impl Named for Target {
  fn name(self: &Target) -> i32 {
    ret self.base
  }
}

impl Mapper < Target > for Source {
  fn map<U>(self: &Source, other: &Target, fill: &U) -> i32 {
    ret self.base + other.name()
  }
}

fn main() -> i32 {
  s := Source { base: 1 }
  t := Target { base: 2 }
  n := 3i32
  println("mapped {}", s.map::<i32>(&t, &n))
  ret 0
}
