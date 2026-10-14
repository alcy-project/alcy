pub struct Point61 {
  pub x: i32,
  pub y: i32,
}

impl Point61 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape61 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area61(s: Shape61) -> i32 {
  match s {
    Shape61::Circle(r) => { ret r * 3 * r }
    Shape61::Rect(w, h) => { ret w * h }
    Shape61::Empty => { ret 0 }
  }
}

pub fn walk61(p: Point61) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
