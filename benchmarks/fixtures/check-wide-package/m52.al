struct Point52 {
  x: i32,
  y: i32,
}

impl Point52 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape52 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area52(s: Shape52) -> i32 {
  match s {
    Shape52::Circle(r) => { ret r * 3 * r }
    Shape52::Rect(w, h) => { ret w * h }
    Shape52::Empty => { ret 0 }
  }
}

fn walk52(p: Point52) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
