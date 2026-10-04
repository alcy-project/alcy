struct Point23 {
  x: i32,
  y: i32,
}

impl Point23 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape23 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area23(s: Shape23) -> i32 {
  match s {
    Shape23::Circle(r) => { ret r * 3 * r }
    Shape23::Rect(w, h) => { ret w * h }
    Shape23::Empty => { ret 0 }
  }
}

fn walk23(p: Point23) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
