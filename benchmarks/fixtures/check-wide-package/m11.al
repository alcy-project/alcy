struct Point11 {
  x: i32,
  y: i32,
}

impl Point11 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape11 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area11(s: Shape11) -> i32 {
  match s {
    Shape11::Circle(r) => { ret r * 3 * r }
    Shape11::Rect(w, h) => { ret w * h }
    Shape11::Empty => { ret 0 }
  }
}

fn walk11(p: Point11) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
