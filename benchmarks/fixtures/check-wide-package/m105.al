struct Point105 {
  x: i32,
  y: i32,
}

impl Point105 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape105 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area105(s: Shape105) -> i32 {
  match s {
    Shape105::Circle(r) => { ret r * 3 * r }
    Shape105::Rect(w, h) => { ret w * h }
    Shape105::Empty => { ret 0 }
  }
}

fn walk105(p: Point105) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
