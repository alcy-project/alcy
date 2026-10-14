pub struct Point20 {
  pub x: i32,
  pub y: i32,
}

impl Point20 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape20 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area20(s: Shape20) -> i32 {
  match s {
    Shape20::Circle(r) => { ret r * 3 * r }
    Shape20::Rect(w, h) => { ret w * h }
    Shape20::Empty => { ret 0 }
  }
}

pub fn walk20(p: Point20) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
