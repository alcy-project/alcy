// A container accessor that answers an owned element ends the borrow at
// the answer: the copy is made of scalars, so it carries no loan and
// the container mutates again while the answer is still live. The
// sanitized run is what checks the frees.
struct Point { x: i64, y: i64 }

enum Shape { Circle(i64), Square(i64), Dot }

fn main() -> i32 {
  // A struct of scalars.
  mut ps := Vec::<Point>::new()
  ps.push(Point { x: 1, y: 2 })
  ps.push(Point { x: 3, y: 4 })
  mid := ps.remove(1)
  ps.push(Point { x: 5, y: 6 })
  if mid.x != 3 { ret 1 }
  if mid.y != 4 { ret 2 }
  if ps.len() != 2 { ret 3 }
  top := ps.pop()
  ps.insert(0, Point { x: 7, y: 8 })
  match top {
    Option::Some(p) => {
      if p.x != 5 { ret 4 }
      if p.y != 6 { ret 5 }
    }
    Option::None => { ret 6 }
  }
  if ps.len() != 2 { ret 7 }

  // A tuple of scalars.
  mut ts := Vec::<(i64, i64)>::new()
  ts.push((10, 20))
  got := ts.pop()
  ts.push((30, 40))
  match got {
    Option::Some((a, b)) => {
      if a + b != 30 { ret 8 }
    }
    Option::None => { ret 9 }
  }

  // An enum of scalars.
  mut ss := Vec::<Shape>::new()
  ss.push(Shape::Circle(7))
  shape := ss.pop()
  ss.push(Shape::Dot)
  match shape {
    Option::Some(s) => {
      match s {
        Shape::Circle(r) => { if r != 7 { ret 10 } }
        _ => { ret 11 }
      }
    }
    Option::None => { ret 12 }
  }

  // An Option of scalars.
  mut os := Vec::<Option<i32>>::new()
  os.push(Option::Some(3))
  inner := os.pop()
  os.push(Option::None)
  match inner {
    Option::Some(Option::Some(x)) => { if x != 3 { ret 13 } }
    Option::Some(Option::None) => { ret 14 }
    Option::None => { ret 15 }
  }
  ret 0
}
