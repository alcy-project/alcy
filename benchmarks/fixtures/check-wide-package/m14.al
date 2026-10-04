struct Point14 {
  x: i32,
  y: i32,
}

impl Point14 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape14 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area14(s: Shape14) -> i32 {
  match s {
    Shape14::Circle(r) => { ret r * 3 * r }
    Shape14::Rect(w, h) => { ret w * h }
    Shape14::Empty => { ret 0 }
  }
}

fn walk14(p: Point14) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
