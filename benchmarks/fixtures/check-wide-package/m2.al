pub struct Point2 {
  pub x: i32,
  pub y: i32,
}

impl Point2 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape2 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area2(s: Shape2) -> i32 {
  match s {
    Shape2::Circle(r) => { ret r * 3 * r }
    Shape2::Rect(w, h) => { ret w * h }
    Shape2::Empty => { ret 0 }
  }
}

pub fn walk2(p: Point2) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
