struct Point24 {
  x: i32,
  y: i32,
}

impl Point24 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape24 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area24(s: Shape24) -> i32 {
  match s {
    Shape24::Circle(r) => { ret r * 3 * r }
    Shape24::Rect(w, h) => { ret w * h }
    Shape24::Empty => { ret 0 }
  }
}

fn walk24(p: Point24) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
