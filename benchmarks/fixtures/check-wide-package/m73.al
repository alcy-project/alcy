struct Point73 {
  x: i32,
  y: i32,
}

impl Point73 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape73 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area73(s: Shape73) -> i32 {
  match s {
    Shape73::Circle(r) => { ret r * 3 * r }
    Shape73::Rect(w, h) => { ret w * h }
    Shape73::Empty => { ret 0 }
  }
}

fn walk73(p: Point73) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
