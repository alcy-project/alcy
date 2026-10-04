struct Point79 {
  x: i32,
  y: i32,
}

impl Point79 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape79 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area79(s: Shape79) -> i32 {
  match s {
    Shape79::Circle(r) => { ret r * 3 * r }
    Shape79::Rect(w, h) => { ret w * h }
    Shape79::Empty => { ret 0 }
  }
}

fn walk79(p: Point79) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
