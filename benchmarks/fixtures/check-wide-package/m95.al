pub struct Point95 {
  pub x: i32,
  pub y: i32,
}

impl Point95 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape95 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area95(s: Shape95) -> i32 {
  match s {
    Shape95::Circle(r) => {
      ret r * 3 * r
    }
    Shape95::Rect(w, h) => {
      ret w * h
    }
    Shape95::Empty => {
      ret 0
    }
  }
}

pub fn walk95(p: Point95) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
