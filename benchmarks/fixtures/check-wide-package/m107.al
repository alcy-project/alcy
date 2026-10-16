pub struct Point107 {
  pub x: i32,
  pub y: i32,
}

impl Point107 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape107 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area107(s: Shape107) -> i32 {
  match s {
    Shape107::Circle(r) => {
      ret r * 3 * r
    }
    Shape107::Rect(w, h) => {
      ret w * h
    }
    Shape107::Empty => {
      ret 0
    }
  }
}

pub fn walk107(p: Point107) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
