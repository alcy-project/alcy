struct Point5 {
  x: i32,
  y: i32,
}

impl Point5 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape5 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area5(s: Shape5) -> i32 {
  match s {
    Shape5::Circle(r) => { ret r * 3 * r }
    Shape5::Rect(w, h) => { ret w * h }
    Shape5::Empty => { ret 0 }
  }
}

fn walk5(p: Point5) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
