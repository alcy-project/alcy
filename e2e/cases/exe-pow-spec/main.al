// A nominal type reaches `**` through the sealed `Pow` spec, the
// same way the other arithmetic reaches its own, and the compound
// form stores the spec's result back into the place.

struct Vec2 {
  x: i32,
  y: i32,
}

impl Pow for Vec2 {
  fn pow(self: Vec2, other: Vec2) -> Vec2 {
    ret Vec2 { x: self.x ** other.x, y: self.y ** other.y }
  }
}

fn main() -> i32 {
  a := Vec2 { x: 2, y: 3 }
  b := Vec2 { x: 5, y: 3 }
  power := a ** b
  if power.x != 32 {
    ret 1
  }
  if power.y != 27 {
    ret 2
  }
  mut accrued := Vec2 { x: 2, y: 3 }
  accrued **= Vec2 { x: 4, y: 2 }
  if accrued.x != 16 {
    ret 3
  }
  if accrued.y != 9 {
    ret 4
  }
  ret 0
}
