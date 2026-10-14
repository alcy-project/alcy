pub struct Point94 {
  pub x: i32,
  pub y: i32,
}

impl Point94 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape94 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area94(s: Shape94) -> i32 {
  match s {
    Shape94::Circle(r) => { ret r * 3 * r }
    Shape94::Rect(w, h) => { ret w * h }
    Shape94::Empty => { ret 0 }
  }
}

pub fn walk94(p: Point94) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
