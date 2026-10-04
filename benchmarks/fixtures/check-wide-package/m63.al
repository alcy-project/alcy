struct Point63 {
  x: i32,
  y: i32,
}

impl Point63 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape63 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area63(s: Shape63) -> i32 {
  match s {
    Shape63::Circle(r) => { ret r * 3 * r }
    Shape63::Rect(w, h) => { ret w * h }
    Shape63::Empty => { ret 0 }
  }
}

fn walk63(p: Point63) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
