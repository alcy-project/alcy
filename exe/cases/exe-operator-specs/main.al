// A nominal type reaches the arithmetic operators through the specs
// in `core`: each operand passes by value, so the operator consumes
// both. The compound forms (`acc += b`) run the binary spec and store
// its result back into the place.

struct Vec2 {
  x: i32,
  y: i32,
}

impl Add for Vec2 {
  fn add(self: Vec2, other: Vec2) -> Vec2 {
    ret Vec2 { x: self.x + other.x, y: self.y + other.y }
  }
}

impl Sub for Vec2 {
  fn sub(self: Vec2, other: Vec2) -> Vec2 {
    ret Vec2 { x: self.x - other.x, y: self.y - other.y }
  }
}

impl Mul for Vec2 {
  fn mul(self: Vec2, other: Vec2) -> Vec2 {
    ret Vec2 { x: self.x * other.x, y: self.y * other.y }
  }
}

impl Div for Vec2 {
  fn div(self: Vec2, other: Vec2) -> Vec2 {
    ret Vec2 { x: self.x / other.x, y: self.y / other.y }
  }
}

impl Rem for Vec2 {
  fn rem(self: Vec2, other: Vec2) -> Vec2 {
    ret Vec2 { x: self.x % other.x, y: self.y % other.y }
  }
}

impl Neg for Vec2 {
  fn neg(self: Vec2) -> Vec2 {
    ret Vec2 { x: -self.x, y: -self.y }
  }
}

fn main() -> i32 {
  a := Vec2 { x: 1, y: 2 }
  b := Vec2 { x: 3, y: 5 }
  sum := a + b
  println("{} {}", sum.x, sum.y)
  difference := b - a
  println("{} {}", difference.x, difference.y)
  product := a * b
  println("{} {}", product.x, product.y)
  quotient := b / a
  println("{} {}", quotient.x, quotient.y)
  remainder := b % a
  println("{} {}", remainder.x, remainder.y)
  negated := -a
  println("{} {}", negated.x, negated.y)
  mut accrued := Vec2 { x: 1, y: 2 }
  accrued += b
  accrued *= Vec2 { x: 2, y: 2 }
  println("{} {}", accrued.x, accrued.y)
  ret 0
}
