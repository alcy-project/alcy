pub struct Point12 {
  pub x: i32,
  pub y: i32,
}

impl Point12 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape12 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area12(s: Shape12) -> i32 {
  match s {
    Shape12::Circle(r) => {
      ret r * 3 * r
    }
    Shape12::Rect(w, h) => {
      ret w * h
    }
    Shape12::Empty => {
      ret 0
    }
  }
}

pub fn walk12(p: Point12) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
