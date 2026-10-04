struct Point8 {
  x: i32,
  y: i32,
}

impl Point8 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape8 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area8(s: Shape8) -> i32 {
  match s {
    Shape8::Circle(r) => { ret r * 3 * r }
    Shape8::Rect(w, h) => { ret w * h }
    Shape8::Empty => { ret 0 }
  }
}

fn walk8(p: Point8) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
