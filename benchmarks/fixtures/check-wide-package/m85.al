pub struct Point85 {
  pub x: i32,
  pub y: i32,
}

impl Point85 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape85 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area85(s: Shape85) -> i32 {
  match s {
    Shape85::Circle(r) => {
      ret r * 3 * r
    }
    Shape85::Rect(w, h) => {
      ret w * h
    }
    Shape85::Empty => {
      ret 0
    }
  }
}

pub fn walk85(p: Point85) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
