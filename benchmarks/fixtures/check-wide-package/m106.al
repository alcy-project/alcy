pub struct Point106 {
  pub x: i32,
  pub y: i32,
}

impl Point106 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape106 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area106(s: Shape106) -> i32 {
  match s {
    Shape106::Circle(r) => { ret r * 3 * r }
    Shape106::Rect(w, h) => { ret w * h }
    Shape106::Empty => { ret 0 }
  }
}

pub fn walk106(p: Point106) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
