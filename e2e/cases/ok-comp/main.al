fn double(comp n: i32) -> i32 {
  ret n * 2
}

fn main() {
  comp k := 21
  _ := double(k)
  _ := comp { 1 + 2 }
}
