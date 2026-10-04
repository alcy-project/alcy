struct Point109 {
  x: i32,
  y: i32,
}

impl Point109 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape109 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area109(s: Shape109) -> i32 {
  match s {
    Shape109::Circle(r) => { ret r * 3 * r }
    Shape109::Rect(w, h) => { ret w * h }
    Shape109::Empty => { ret 0 }
  }
}

fn walk109(p: Point109) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
