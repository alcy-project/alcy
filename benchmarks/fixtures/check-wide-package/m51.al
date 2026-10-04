struct Point51 {
  x: i32,
  y: i32,
}

impl Point51 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape51 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area51(s: Shape51) -> i32 {
  match s {
    Shape51::Circle(r) => { ret r * 3 * r }
    Shape51::Rect(w, h) => { ret w * h }
    Shape51::Empty => { ret 0 }
  }
}

fn walk51(p: Point51) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
