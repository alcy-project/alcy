struct Point86 {
  x: i32,
  y: i32,
}

impl Point86 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape86 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area86(s: Shape86) -> i32 {
  match s {
    Shape86::Circle(r) => { ret r * 3 * r }
    Shape86::Rect(w, h) => { ret w * h }
    Shape86::Empty => { ret 0 }
  }
}

fn walk86(p: Point86) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
