struct Point83 {
  x: i32,
  y: i32,
}

impl Point83 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape83 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area83(s: Shape83) -> i32 {
  match s {
    Shape83::Circle(r) => { ret r * 3 * r }
    Shape83::Rect(w, h) => { ret w * h }
    Shape83::Empty => { ret 0 }
  }
}

fn walk83(p: Point83) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
