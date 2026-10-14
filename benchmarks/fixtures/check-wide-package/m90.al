pub struct Point90 {
  pub x: i32,
  pub y: i32,
}

impl Point90 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape90 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area90(s: Shape90) -> i32 {
  match s {
    Shape90::Circle(r) => { ret r * 3 * r }
    Shape90::Rect(w, h) => { ret w * h }
    Shape90::Empty => { ret 0 }
  }
}

pub fn walk90(p: Point90) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
