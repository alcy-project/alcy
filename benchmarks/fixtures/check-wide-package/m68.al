pub struct Point68 {
  pub x: i32,
  pub y: i32,
}

impl Point68 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape68 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area68(s: Shape68) -> i32 {
  match s {
    Shape68::Circle(r) => { ret r * 3 * r }
    Shape68::Rect(w, h) => { ret w * h }
    Shape68::Empty => { ret 0 }
  }
}

pub fn walk68(p: Point68) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
