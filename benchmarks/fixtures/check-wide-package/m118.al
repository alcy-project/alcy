struct Point118 {
  x: i32,
  y: i32,
}

impl Point118 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape118 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area118(s: Shape118) -> i32 {
  match s {
    Shape118::Circle(r) => { ret r * 3 * r }
    Shape118::Rect(w, h) => { ret w * h }
    Shape118::Empty => { ret 0 }
  }
}

fn walk118(p: Point118) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
