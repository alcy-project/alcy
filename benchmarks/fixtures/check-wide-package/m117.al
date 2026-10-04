struct Point117 {
  x: i32,
  y: i32,
}

impl Point117 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape117 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area117(s: Shape117) -> i32 {
  match s {
    Shape117::Circle(r) => { ret r * 3 * r }
    Shape117::Rect(w, h) => { ret w * h }
    Shape117::Empty => { ret 0 }
  }
}

fn walk117(p: Point117) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
