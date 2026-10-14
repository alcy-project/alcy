// A spec method may declare type parameters of its own. The call
// spells them, the impl's signature compares against the declaration
// by position rather than by name, and the bound is checked where the
// call instantiates.

spec Mark {
  fn mark(self: &Self) -> i32;
}

struct Point {
  x: i32,
  y: i32,
}

impl Mark for Point {
  fn mark(self: &Self) -> i32 {
    ret self.x + self.y
  }
}

spec Take {
  fn take<T>(self: &Self, value: T) -> T;
  fn total<T: Mark>(self: &Self, value: T) -> i32;
}

impl Take for Point {
  fn take<U>(self: &Self, value: U) -> U {
    ret value
  }

  fn total<V: Mark>(self: &Self, value: V) -> i32 {
    ret self.mark() + value.mark()
  }
}

fn main() -> i32 {
  p := Point { x: 1, y: 2 }
  if p.take::<i32>(41) != 41 {
    ret 1
  }
  q := Point { x: 3, y: 4 }
  if p.total::<Point>(q) != 10 {
    ret 2
  }
  moved := p.take::<String>(p.take::<String>(String::new()))
  if moved.len() != 0 {
    ret 3
  }
  println("{}", p.total::<Point>(Point { x: 1, y: 1 }))
  ret 0
}
