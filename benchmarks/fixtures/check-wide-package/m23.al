pub struct Point23 {
  pub x: i32,
  pub y: i32,
}

impl Point23 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape23 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area23(s: Shape23) -> i32 {
  match s {
    Shape23::Circle(r) => { ret r * 3 * r }
    Shape23::Rect(w, h) => { ret w * h }
    Shape23::Empty => { ret 0 }
  }
}

pub fn walk23(p: Point23) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
