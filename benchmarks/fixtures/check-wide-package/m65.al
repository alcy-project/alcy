struct Point65 {
  x: i32,
  y: i32,
}

impl Point65 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape65 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area65(s: Shape65) -> i32 {
  match s {
    Shape65::Circle(r) => { ret r * 3 * r }
    Shape65::Rect(w, h) => { ret w * h }
    Shape65::Empty => { ret 0 }
  }
}

fn walk65(p: Point65) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
