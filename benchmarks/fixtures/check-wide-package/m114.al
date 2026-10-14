pub struct Point114 {
  pub x: i32,
  pub y: i32,
}

impl Point114 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape114 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area114(s: Shape114) -> i32 {
  match s {
    Shape114::Circle(r) => { ret r * 3 * r }
    Shape114::Rect(w, h) => { ret w * h }
    Shape114::Empty => { ret 0 }
  }
}

pub fn walk114(p: Point114) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
