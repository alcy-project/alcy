struct Point84 {
  x: i32,
  y: i32,
}

impl Point84 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape84 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area84(s: Shape84) -> i32 {
  match s {
    Shape84::Circle(r) => { ret r * 3 * r }
    Shape84::Rect(w, h) => { ret w * h }
    Shape84::Empty => { ret 0 }
  }
}

fn walk84(p: Point84) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
