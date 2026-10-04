struct Point27 {
  x: i32,
  y: i32,
}

impl Point27 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape27 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area27(s: Shape27) -> i32 {
  match s {
    Shape27::Circle(r) => { ret r * 3 * r }
    Shape27::Rect(w, h) => { ret w * h }
    Shape27::Empty => { ret 0 }
  }
}

fn walk27(p: Point27) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
