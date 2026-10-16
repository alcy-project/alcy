pub struct Point18 {
  pub x: i32,
  pub y: i32,
}

impl Point18 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape18 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area18(s: Shape18) -> i32 {
  match s {
    Shape18::Circle(r) => {
      ret r * 3 * r
    }
    Shape18::Rect(w, h) => {
      ret w * h
    }
    Shape18::Empty => {
      ret 0
    }
  }
}

pub fn walk18(p: Point18) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
