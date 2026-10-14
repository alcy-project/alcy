pub struct Point83 {
  pub x: i32,
  pub y: i32,
}

impl Point83 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape83 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area83(s: Shape83) -> i32 {
  match s {
    Shape83::Circle(r) => { ret r * 3 * r }
    Shape83::Rect(w, h) => { ret w * h }
    Shape83::Empty => { ret 0 }
  }
}

pub fn walk83(p: Point83) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
