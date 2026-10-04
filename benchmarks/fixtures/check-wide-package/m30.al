struct Point30 {
  x: i32,
  y: i32,
}

impl Point30 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape30 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area30(s: Shape30) -> i32 {
  match s {
    Shape30::Circle(r) => { ret r * 3 * r }
    Shape30::Rect(w, h) => { ret w * h }
    Shape30::Empty => { ret 0 }
  }
}

fn walk30(p: Point30) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
