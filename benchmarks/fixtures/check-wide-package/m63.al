pub struct Point63 {
  pub x: i32,
  pub y: i32,
}

impl Point63 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape63 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area63(s: Shape63) -> i32 {
  match s {
    Shape63::Circle(r) => {
      ret r * 3 * r
    }
    Shape63::Rect(w, h) => {
      ret w * h
    }
    Shape63::Empty => {
      ret 0
    }
  }
}

pub fn walk63(p: Point63) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
