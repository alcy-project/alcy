pub struct Point17 {
  pub x: i32,
  pub y: i32,
}

impl Point17 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape17 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area17(s: Shape17) -> i32 {
  match s {
    Shape17::Circle(r) => { ret r * 3 * r }
    Shape17::Rect(w, h) => { ret w * h }
    Shape17::Empty => { ret 0 }
  }
}

pub fn walk17(p: Point17) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
