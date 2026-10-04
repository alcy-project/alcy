struct Point66 {
  x: i32,
  y: i32,
}

impl Point66 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape66 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area66(s: Shape66) -> i32 {
  match s {
    Shape66::Circle(r) => { ret r * 3 * r }
    Shape66::Rect(w, h) => { ret w * h }
    Shape66::Empty => { ret 0 }
  }
}

fn walk66(p: Point66) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
