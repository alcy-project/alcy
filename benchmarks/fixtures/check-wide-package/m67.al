pub struct Point67 {
  pub x: i32,
  pub y: i32,
}

impl Point67 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape67 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area67(s: Shape67) -> i32 {
  match s {
    Shape67::Circle(r) => {
      ret r * 3 * r
    }
    Shape67::Rect(w, h) => {
      ret w * h
    }
    Shape67::Empty => {
      ret 0
    }
  }
}

pub fn walk67(p: Point67) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
