pub struct Point37 {
  pub x: i32,
  pub y: i32,
}

impl Point37 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape37 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area37(s: Shape37) -> i32 {
  match s {
    Shape37::Circle(r) => {
      ret r * 3 * r
    }
    Shape37::Rect(w, h) => {
      ret w * h
    }
    Shape37::Empty => {
      ret 0
    }
  }
}

pub fn walk37(p: Point37) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
