// A method answers from the block that declares it: a `Vec` of a type
// that is not `Ord` iterates, even though a later block asks `T: Ord`
// for the sorting methods. A block that does not hold the name is not
// this call's to fail.
struct Point {
  x: i32,
  y: i32,
}

fn main() -> i32 {
  mut v := Vec::<Point>::new()
  v.push(Point { x: 1, y: 2 })
  v.push(Point { x: 3, y: 4 })
  mut n := 0
  for p in v.iter() {
    n = n + p.x + p.y
  }
  if n != 10 {
    ret 1
  }
  mut f := Vec::<f64>::new()
  f.push(1.5)
  f.push(2.5)
  mut s := 0.0
  for x in f.iter() {
    s = s + *x
  }
  if s != 4.0 {
    ret 2
  }
  ret 0
}
