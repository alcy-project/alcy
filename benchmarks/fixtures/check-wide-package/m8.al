pub struct Point8 {
  pub x: i32,
  pub y: i32,
}

impl Point8 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape8 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area8(s: Shape8) -> i32 {
  match s {
    Shape8::Circle(r) => {
      ret r * 3 * r
    }
    Shape8::Rect(w, h) => {
      ret w * h
    }
    Shape8::Empty => {
      ret 0
    }
  }
}

pub fn walk8(p: Point8) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
