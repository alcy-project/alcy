pub struct Point38 {
  pub x: i32,
  pub y: i32,
}

impl Point38 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape38 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area38(s: Shape38) -> i32 {
  match s {
    Shape38::Circle(r) => { ret r * 3 * r }
    Shape38::Rect(w, h) => { ret w * h }
    Shape38::Empty => { ret 0 }
  }
}

pub fn walk38(p: Point38) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
