pub struct Point110 {
  pub x: i32,
  pub y: i32,
}

impl Point110 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape110 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area110(s: Shape110) -> i32 {
  match s {
    Shape110::Circle(r) => {
      ret r * 3 * r
    }
    Shape110::Rect(w, h) => {
      ret w * h
    }
    Shape110::Empty => {
      ret 0
    }
  }
}

pub fn walk110(p: Point110) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
