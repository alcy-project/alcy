fn collect() -> Vec<&i32> {
  value := 7i32
  mut out := Vec::<&i32>::new()
  out.push(&value)
  ret out
}

fn main() -> i32 {
  out := collect()
  ret out.len() as i32 - 1
}
