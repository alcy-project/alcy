struct Point22 {
  x: i32,
  y: i32,
}

impl Point22 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape22 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area22(s: Shape22) -> i32 {
  match s {
    Shape22::Circle(r) => { ret r * 3 * r }
    Shape22::Rect(w, h) => { ret w * h }
    Shape22::Empty => { ret 0 }
  }
}

fn walk22(p: Point22) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
