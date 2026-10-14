pub struct Point115 {
  pub x: i32,
  pub y: i32,
}

impl Point115 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape115 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area115(s: Shape115) -> i32 {
  match s {
    Shape115::Circle(r) => { ret r * 3 * r }
    Shape115::Rect(w, h) => { ret w * h }
    Shape115::Empty => { ret 0 }
  }
}

pub fn walk115(p: Point115) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
