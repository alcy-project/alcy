// The specification's parameter declares a 'Named' bound, and the
// argument the implementation names does not implement it. A method with
// type parameters of its own still answers its call through the same
// declaration check.
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

impl Mapper < Target > for Source {
  fn map<U>(self: &Source, other: &Target, fill: &U) -> i32 {
    ret self.base + other.base
  }
}

fn main() -> i32 {
  s := Source { base: 1 }
  t := Target { base: 2 }
  n := 3i32
  println("{}", s.map::<i32>(&t, &n))
  ret 0
}
