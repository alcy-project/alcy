pub struct Point6 {
  pub x: i32,
  pub y: i32,
}

impl Point6 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape6 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area6(s: Shape6) -> i32 {
  match s {
    Shape6::Circle(r) => { ret r * 3 * r }
    Shape6::Rect(w, h) => { ret w * h }
    Shape6::Empty => { ret 0 }
  }
}

pub fn walk6(p: Point6) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
