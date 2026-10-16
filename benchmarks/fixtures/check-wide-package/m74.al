pub struct Point74 {
  pub x: i32,
  pub y: i32,
}

impl Point74 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape74 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area74(s: Shape74) -> i32 {
  match s {
    Shape74::Circle(r) => {
      ret r * 3 * r
    }
    Shape74::Rect(w, h) => {
      ret w * h
    }
    Shape74::Empty => {
      ret 0
    }
  }
}

pub fn walk74(p: Point74) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
