pub struct Point40 {
  pub x: i32,
  pub y: i32,
}

impl Point40 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape40 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area40(s: Shape40) -> i32 {
  match s {
    Shape40::Circle(r) => { ret r * 3 * r }
    Shape40::Rect(w, h) => { ret w * h }
    Shape40::Empty => { ret 0 }
  }
}

pub fn walk40(p: Point40) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
