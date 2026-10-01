// Specs declare capabilities and impls opt types in: dispatch
// tries inherent methods first, then the in-scope spec impls.
// A generic spec instantiates per target, and a generic impl
// specializes per call.
use self::shapes::Point;
use self::shapes::Show;
use self::shapes::Wrap;

spec Iterator<T> {
  fn next(mut self: &mut Self) -> Option<T>;
}

struct Counter {
  n: i32,
}

impl Iterator<i32> for Counter {
  fn next(mut self: &mut Self) -> Option<i32> {
    self.n = self.n + 1
    ret Option::Some(self.n)
  }
}

spec Build {
  fn make() -> Self;
}

impl Build for Point {
  fn make() -> Point {
    ret Point { x: 1, y: 2 }
  }
}

struct Local {
  x: i32,
}

impl Local {
  fn show(self: &Self) -> str {
    ret "inherent"
  }
}

spec LocalShow {
  fn show(self: &Self) -> str;
}

impl LocalShow for Local {
  fn show(self: &Self) -> str {
    ret "spec"
  }
}

fn main() -> i32 {
  mut bad := 0
  // A spec method through an import, beside the inherent twin.
  p := Point { x: 3, y: 4 }
  if str_len(p.label()) != 7 {
    bad = 1
  }
  // A generic spec over a local type.
  mut c := Counter { n: 0 }
  if c.next().unwrap() != 1 {
    bad = 2
  }
  if c.next().unwrap() != 2 {
    bad = 3
  }
  // A generic impl specializes per instantiation.
  w: Wrap<i32> := Wrap { v: 41 }
  if str_len(w.show()) != 7 {
    bad = 4
  }
  // An associated function of a spec.
  q := Point::make()
  if q.x != 1 {
    bad = 5
  }
  // An inherent method wins over a spec method of the same name.
  t := Local { x: 0 }
  if str_len(t.show()) != 8 {
    bad = 6
  }
  ret bad
}
