struct Point88 {
  x: i32,
  y: i32,
}

impl Point88 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape88 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area88(s: Shape88) -> i32 {
  match s {
    Shape88::Circle(r) => { ret r * 3 * r }
    Shape88::Rect(w, h) => { ret w * h }
    Shape88::Empty => { ret 0 }
  }
}

fn walk88(p: Point88) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
