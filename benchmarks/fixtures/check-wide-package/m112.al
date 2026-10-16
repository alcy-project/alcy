pub struct Point112 {
  pub x: i32,
  pub y: i32,
}

impl Point112 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape112 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area112(s: Shape112) -> i32 {
  match s {
    Shape112::Circle(r) => {
      ret r * 3 * r
    }
    Shape112::Rect(w, h) => {
      ret w * h
    }
    Shape112::Empty => {
      ret 0
    }
  }
}

pub fn walk112(p: Point112) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
