fn double(comp n: i32) -> i32 {
  ret n * 2
}

fn sum_to(comp n: i32) -> i32 {
  ret comp {
    mut t := 0
    mut i := 1
    while i <= n {
      t = t + i
      i = i + 1
    }
    t
  }
}

fn main() -> i32 {
  comp k := 21
  ret double(k) + sum_to(4) - comp { 1 + 2 }
}
