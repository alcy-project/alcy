pub struct Point48 {
  pub x: i32,
  pub y: i32,
}

impl Point48 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape48 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area48(s: Shape48) -> i32 {
  match s {
    Shape48::Circle(r) => { ret r * 3 * r }
    Shape48::Rect(w, h) => { ret w * h }
    Shape48::Empty => { ret 0 }
  }
}

pub fn walk48(p: Point48) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
