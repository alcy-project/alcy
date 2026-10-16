pub struct Point76 {
  pub x: i32,
  pub y: i32,
}

impl Point76 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape76 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area76(s: Shape76) -> i32 {
  match s {
    Shape76::Circle(r) => {
      ret r * 3 * r
    }
    Shape76::Rect(w, h) => {
      ret w * h
    }
    Shape76::Empty => {
      ret 0
    }
  }
}

pub fn walk76(p: Point76) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
