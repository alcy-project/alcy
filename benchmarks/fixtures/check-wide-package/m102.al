struct Point102 {
  x: i32,
  y: i32,
}

impl Point102 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape102 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area102(s: Shape102) -> i32 {
  match s {
    Shape102::Circle(r) => { ret r * 3 * r }
    Shape102::Rect(w, h) => { ret w * h }
    Shape102::Empty => { ret 0 }
  }
}

fn walk102(p: Point102) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
