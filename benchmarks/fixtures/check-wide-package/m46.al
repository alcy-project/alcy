struct Point46 {
  x: i32,
  y: i32,
}

impl Point46 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape46 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area46(s: Shape46) -> i32 {
  match s {
    Shape46::Circle(r) => { ret r * 3 * r }
    Shape46::Rect(w, h) => { ret w * h }
    Shape46::Empty => { ret 0 }
  }
}

fn walk46(p: Point46) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
