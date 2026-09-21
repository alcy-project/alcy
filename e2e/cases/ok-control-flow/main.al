enum Shape { Circle(i32), Square(i32), Rect }

fn area(s: Shape) -> i32 {
  r := match s {
    Shape::Circle(x) | Shape::Square(x) => x,
    Shape::Rect => 0,
  }
  ret r
}

fn search(limit: i32) -> i32 {
  mut i := 0
  mut found := 0
  while i < limit {
    i = i + 1
    loop {
      found = found + 1
      if found > 10 {
        break
      }
      if found == 3 {
        continue
      }
    }
    if Some(v) := Some(i) {
      found = found + v
    }
  }
  ret found
}

fn calc(o: Option<i32>) -> Option<i32> {
  v := o?
  ret Some(v + 1)
}

fn main() {
  a := area(Shape::Square(4))
  b := search(3)
  v := if a > b {
    a
  } else {
    b
  }
  o: Option<i32> := Some(v)
  r := calc(o)
  _ := r
}
