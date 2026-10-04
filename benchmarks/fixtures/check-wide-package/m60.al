struct Point60 {
  x: i32,
  y: i32,
}

impl Point60 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape60 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area60(s: Shape60) -> i32 {
  match s {
    Shape60::Circle(r) => { ret r * 3 * r }
    Shape60::Rect(w, h) => { ret w * h }
    Shape60::Empty => { ret 0 }
  }
}

fn walk60(p: Point60) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
