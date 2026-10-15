// An integer dot product over two vectors, the shorter length deciding
// how far the walk goes.
fn dot(a: &Vec<i64>, b: &Vec<i64>) -> i64 {
  mut n := a.len()
  if b.len() < n {
    n = b.len()
  }
  mut s := 0 as i64
  mut i := 0 as usize
  while i < n {
    s = s + *a.at(i).unwrap() * *b.at(i).unwrap()
    i = i + 1
  }
  ret s
}

fn main() -> i32 {
  mut a := Vec::<i64>::new()
  a.push(1)
  a.push(2)
  a.push(3)
  mut b := Vec::<i64>::new()
  b.push(4)
  b.push(5)
  if dot(&a, &b) != 14 { ret 1 }
  ret 0
}
