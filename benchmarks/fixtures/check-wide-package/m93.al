pub struct Point93 {
  pub x: i32,
  pub y: i32,
}

impl Point93 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape93 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area93(s: Shape93) -> i32 {
  match s {
    Shape93::Circle(r) => { ret r * 3 * r }
    Shape93::Rect(w, h) => { ret w * h }
    Shape93::Empty => { ret 0 }
  }
}

pub fn walk93(p: Point93) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
