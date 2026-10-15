// A method that answers an owned element ends the receiver's borrow at
// the answer when the element is made of scalars: the copy carries no
// loan, so the container mutates again while the answer is live.
struct Point { x: i64, y: i64 }

fn main() -> i32 {
  mut ps := Vec::<Point>::new()
  ps.push(Point { x: 1, y: 2 })
  d := ps.pop()
  ps.push(Point { x: 3, y: 4 })
  match d {
    Option::Some(p) => {
      if p.x != 1 { ret 1 }
    }
    Option::None => { ret 2 }
  }
  ret 0
}
