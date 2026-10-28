// A `const` item names no storage, so borrowing one lends a temporary
// holding its value: the `str` methods, a reference, and a method on a
// constant struct all read it back.
struct Point {
  x: i32,
  y: i32,
}

impl Point {
  pub fn sum(self: &Self) -> i32 {
    ret self.x + self.y
  }
}

const GREETING: str = " hello "

const ORIGIN: Point = Point { x: 3, y: 4 }

const SIZE: i32 = 3

fn main() -> i32 {
  if str_len(GREETING.trim()) != 5 {
    ret 1
  }
  if !(GREETING.byte_slice(1, 4) == "hel") {
    ret 2
  }
  if ORIGIN.sum() != 7 {
    ret 3
  }
  m := &SIZE
  if *m != 3 {
    ret 4
  }
  ret 0
}
