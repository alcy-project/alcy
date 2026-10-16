pub struct Point32 {
  pub x: i32,
  pub y: i32,
}

impl Point32 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape32 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area32(s: Shape32) -> i32 {
  match s {
    Shape32::Circle(r) => {
      ret r * 3 * r
    }
    Shape32::Rect(w, h) => {
      ret w * h
    }
    Shape32::Empty => {
      ret 0
    }
  }
}

pub fn walk32(p: Point32) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
