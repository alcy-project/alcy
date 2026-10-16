pub struct Point43 {
  pub x: i32,
  pub y: i32,
}

impl Point43 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape43 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area43(s: Shape43) -> i32 {
  match s {
    Shape43::Circle(r) => {
      ret r * 3 * r
    }
    Shape43::Rect(w, h) => {
      ret w * h
    }
    Shape43::Empty => {
      ret 0
    }
  }
}

pub fn walk43(p: Point43) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
