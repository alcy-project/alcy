struct Point62 {
  x: i32,
  y: i32,
}

impl Point62 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape62 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area62(s: Shape62) -> i32 {
  match s {
    Shape62::Circle(r) => { ret r * 3 * r }
    Shape62::Rect(w, h) => { ret w * h }
    Shape62::Empty => { ret 0 }
  }
}

fn walk62(p: Point62) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
