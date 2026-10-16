pub struct Point10 {
  pub x: i32,
  pub y: i32,
}

impl Point10 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape10 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area10(s: Shape10) -> i32 {
  match s {
    Shape10::Circle(r) => {
      ret r * 3 * r
    }
    Shape10::Rect(w, h) => {
      ret w * h
    }
    Shape10::Empty => {
      ret 0
    }
  }
}

pub fn walk10(p: Point10) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
