pub struct Point0 {
  pub x: i32,
  pub y: i32,
}

impl Point0 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape0 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area0(s: Shape0) -> i32 {
  match s {
    Shape0::Circle(r) => { ret r * 3 * r }
    Shape0::Rect(w, h) => { ret w * h }
    Shape0::Empty => { ret 0 }
  }
}

pub fn walk0(p: Point0) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
