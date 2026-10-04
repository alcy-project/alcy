struct Point99 {
  x: i32,
  y: i32,
}

impl Point99 {
  fn length_squared(self: Self) -> i32 {
    ret self.x * self.x + self.y * self.y
  }
}

enum Shape99 {
  Circle(i32),
  Rect(i32, i32),
  Empty,
}

fn area99(s: Shape99) -> i32 {
  match s {
    Shape99::Circle(r) => { ret r * 3 * r }
    Shape99::Rect(w, h) => { ret w * h }
    Shape99::Empty => { ret 0 }
  }
}

fn walk99(p: Point99) -> i32 {
  mut total := 0
  mut n := p.x
  while n > 0 {
    total = total + n
    n = n - 1
  }
  ret total + p.length_squared()
}
