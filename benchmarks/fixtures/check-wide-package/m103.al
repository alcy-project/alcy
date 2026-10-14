pub struct Point103 {
  pub x: i32,
  pub y: i32,
}

impl Point103 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape103 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area103(s: Shape103) -> i32 {
  match s {
    Shape103::Circle(r) => { ret r * 3 * r }
    Shape103::Rect(w, h) => { ret w * h }
    Shape103::Empty => { ret 0 }
  }
}

pub fn walk103(p: Point103) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
