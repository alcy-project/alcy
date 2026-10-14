pub struct Point57 {
  pub x: i32,
  pub y: i32,
}

impl Point57 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape57 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area57(s: Shape57) -> i32 {
  match s {
    Shape57::Circle(r) => { ret r * 3 * r }
    Shape57::Rect(w, h) => { ret w * h }
    Shape57::Empty => { ret 0 }
  }
}

pub fn walk57(p: Point57) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
