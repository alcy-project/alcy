pub struct Point58 {
  pub x: i32,
  pub y: i32,
}

impl Point58 {
  pub fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

pub enum Shape58 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

pub fn area58(s: Shape58) -> i32 {
  match s {
    Shape58::Circle(r) => { ret r * 3 * r }
    Shape58::Rect(w, h) => { ret w * h }
    Shape58::Empty => { ret 0 }
  }
}

pub fn walk58(p: Point58) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
