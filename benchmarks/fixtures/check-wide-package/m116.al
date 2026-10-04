struct Point116 {
  x: i32,
  y: i32,
}

impl Point116 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape116 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area116(s: Shape116) -> i32 {
  match s {
    Shape116::Circle(r) => { ret r * 3 * r }
    Shape116::Rect(w, h) => { ret w * h }
    Shape116::Empty => { ret 0 }
  }
}

fn walk116(p: Point116) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
