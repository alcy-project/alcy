pub struct Point13 {
  pub x: i32,
  pub y: i32,
}

impl Point13 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape13 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area13(s: Shape13) -> i32 {
  match s {
    Shape13::Circle(r) => {
      ret r * 3 * r
    }
    Shape13::Rect(w, h) => {
      ret w * h
    }
    Shape13::Empty => {
      ret 0
    }
  }
}

pub fn walk13(p: Point13) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
