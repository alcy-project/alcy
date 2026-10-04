struct Point45 {
  x: i32,
  y: i32,
}

impl Point45 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape45 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area45(s: Shape45) -> i32 {
  match s {
    Shape45::Circle(r) => { ret r * 3 * r }
    Shape45::Rect(w, h) => { ret w * h }
    Shape45::Empty => { ret 0 }
  }
}

fn walk45(p: Point45) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
