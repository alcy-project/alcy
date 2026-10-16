pub struct Point105 {
  pub x: i32,
  pub y: i32,
}

impl Point105 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape105 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area105(s: Shape105) -> i32 {
  match s {
    Shape105::Circle(r) => {
      ret r * 3 * r
    }
    Shape105::Rect(w, h) => {
      ret w * h
    }
    Shape105::Empty => {
      ret 0
    }
  }
}

pub fn walk105(p: Point105) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
