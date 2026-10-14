pub struct Point21 {
  pub x: i32,
  pub y: i32,
}

impl Point21 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape21 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area21(s: Shape21) -> i32 {
  match s {
    Shape21::Circle(r) => { ret r * 3 * r }
    Shape21::Rect(w, h) => { ret w * h }
    Shape21::Empty => { ret 0 }
  }
}

pub fn walk21(p: Point21) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
