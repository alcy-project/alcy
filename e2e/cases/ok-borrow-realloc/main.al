// A loan held across a read-only walk is the shape a container's
// accessors exist for, so it has to keep compiling: borrowing on every
// iteration of a loop over a vector, with no write in sight.
fn sum_all(v: &Vec<i32>) -> i32 {
  mut total := 0i32
  mut i := 0 as usize
  while i < v.len() {
    total = total + *v.at(i).unwrap()
    i = i + 1
  }
  ret total
}

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  mut i := 0i32
  while i < 10 {
    v.push(i)
    i = i + 1
  }
  if sum_all(&v) != 45 {
    ret 1
  }
  // A result kept after the last push, which reallocated before it.
  last := v.at(9).unwrap()
  ret *last - 9
}
