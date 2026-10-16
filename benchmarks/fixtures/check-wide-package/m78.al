pub struct Point78 {
  pub x: i32,
  pub y: i32,
}

impl Point78 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape78 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area78(s: Shape78) -> i32 {
  match s {
    Shape78::Circle(r) => {
      ret r * 3 * r
    }
    Shape78::Rect(w, h) => {
      ret w * h
    }
    Shape78::Empty => {
      ret 0
    }
  }
}

pub fn walk78(p: Point78) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
