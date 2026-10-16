pub struct Point41 {
  pub x: i32,
  pub y: i32,
}

impl Point41 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape41 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area41(s: Shape41) -> i32 {
  match s {
    Shape41::Circle(r) => {
      ret r * 3 * r
    }
    Shape41::Rect(w, h) => {
      ret w * h
    }
    Shape41::Empty => {
      ret 0
    }
  }
}

pub fn walk41(p: Point41) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
