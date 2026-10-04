struct Point56 {
  x: i32,
  y: i32,
}

impl Point56 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape56 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area56(s: Shape56) -> i32 {
  match s {
    Shape56::Circle(r) => { ret r * 3 * r }
    Shape56::Rect(w, h) => { ret w * h }
    Shape56::Empty => { ret 0 }
  }
}

fn walk56(p: Point56) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
