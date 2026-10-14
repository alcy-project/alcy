pub struct Point75 {
  pub x: i32,
  pub y: i32,
}

impl Point75 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape75 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area75(s: Shape75) -> i32 {
  match s {
    Shape75::Circle(r) => { ret r * 3 * r }
    Shape75::Rect(w, h) => { ret w * h }
    Shape75::Empty => { ret 0 }
  }
}

pub fn walk75(p: Point75) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
