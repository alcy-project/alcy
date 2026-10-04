struct Point50 {
  x: i32,
  y: i32,
}

impl Point50 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape50 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area50(s: Shape50) -> i32 {
  match s {
    Shape50::Circle(r) => { ret r * 3 * r }
    Shape50::Rect(w, h) => { ret w * h }
    Shape50::Empty => { ret 0 }
  }
}

fn walk50(p: Point50) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
