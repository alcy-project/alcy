struct Point89 {
  x: i32,
  y: i32,
}

impl Point89 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape89 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area89(s: Shape89) -> i32 {
  match s {
    Shape89::Circle(r) => { ret r * 3 * r }
    Shape89::Rect(w, h) => { ret w * h }
    Shape89::Empty => { ret 0 }
  }
}

fn walk89(p: Point89) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
