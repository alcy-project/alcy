struct Point119 {
  x: i32,
  y: i32,
}

impl Point119 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape119 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area119(s: Shape119) -> i32 {
  match s {
    Shape119::Circle(r) => { ret r * 3 * r }
    Shape119::Rect(w, h) => { ret w * h }
    Shape119::Empty => { ret 0 }
  }
}

fn walk119(p: Point119) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
