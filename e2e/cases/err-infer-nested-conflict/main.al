// Two parameters that nest the same slot must agree: the first binds
// it and the second is checked against that binding.
fn both<T>(a: &Vec<T>, b: &Vec<T>) -> i32 {
  ret 0
}

fn main() -> i32 {
  mut a := Vec::<i32>::new()
  mut b := Vec::<String>::new()
  b.push(String::from("x"))
  ret both(&a, &b)
}
