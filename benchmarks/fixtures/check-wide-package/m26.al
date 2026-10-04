struct Point26 {
  x: i32,
  y: i32,
}

impl Point26 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape26 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area26(s: Shape26) -> i32 {
  match s {
    Shape26::Circle(r) => { ret r * 3 * r }
    Shape26::Rect(w, h) => { ret w * h }
    Shape26::Empty => { ret 0 }
  }
}

fn walk26(p: Point26) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
