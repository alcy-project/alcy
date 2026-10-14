pub struct Point9 {
  pub x: i32,
  pub y: i32,
}

impl Point9 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape9 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area9(s: Shape9) -> i32 {
  match s {
    Shape9::Circle(r) => { ret r * 3 * r }
    Shape9::Rect(w, h) => { ret w * h }
    Shape9::Empty => { ret 0 }
  }
}

pub fn walk9(p: Point9) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
