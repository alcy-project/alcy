struct Point103 {
  x: i32,
  y: i32,
}

impl Point103 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape103 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area103(s: Shape103) -> i32 {
  match s {
    Shape103::Circle(r) => { ret r * 3 * r }
    Shape103::Rect(w, h) => { ret w * h }
    Shape103::Empty => { ret 0 }
  }
}

fn walk103(p: Point103) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
