struct Point18 {
  x: i32,
  y: i32,
}

impl Point18 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape18 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area18(s: Shape18) -> i32 {
  match s {
    Shape18::Circle(r) => { ret r * 3 * r }
    Shape18::Rect(w, h) => { ret w * h }
    Shape18::Empty => { ret 0 }
  }
}

fn walk18(p: Point18) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
