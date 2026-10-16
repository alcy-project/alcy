pub struct Point33 {
  pub x: i32,
  pub y: i32,
}

impl Point33 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape33 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area33(s: Shape33) -> i32 {
  match s {
    Shape33::Circle(r) => {
      ret r * 3 * r
    }
    Shape33::Rect(w, h) => {
      ret w * h
    }
    Shape33::Empty => {
      ret 0
    }
  }
}

pub fn walk33(p: Point33) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
