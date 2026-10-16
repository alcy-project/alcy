pub struct Point55 {
  pub x: i32,
  pub y: i32,
}

impl Point55 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape55 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area55(s: Shape55) -> i32 {
  match s {
    Shape55::Circle(r) => {
      ret r * 3 * r
    }
    Shape55::Rect(w, h) => {
      ret w * h
    }
    Shape55::Empty => {
      ret 0
    }
  }
}

pub fn walk55(p: Point55) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
