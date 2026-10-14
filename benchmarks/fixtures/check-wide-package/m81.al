pub struct Point81 {
  pub x: i32,
  pub y: i32,
}

impl Point81 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape81 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area81(s: Shape81) -> i32 {
  match s {
    Shape81::Circle(r) => { ret r * 3 * r }
    Shape81::Rect(w, h) => { ret w * h }
    Shape81::Empty => { ret 0 }
  }
}

pub fn walk81(p: Point81) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
