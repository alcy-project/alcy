pub struct Point44 {
  pub x: i32,
  pub y: i32,
}

impl Point44 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape44 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area44(s: Shape44) -> i32 {
  match s {
    Shape44::Circle(r) => {
      ret r * 3 * r
    }
    Shape44::Rect(w, h) => {
      ret w * h
    }
    Shape44::Empty => {
      ret 0
    }
  }
}

pub fn walk44(p: Point44) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
