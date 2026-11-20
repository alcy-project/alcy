pub fn display_str(value: str, out: *mut u8, cap: usize) -> usize {
  _ := str_len(value)
  _ := out
  _ := cap
  ret 0
}

pub fn display_u64(value: u64, out: *mut u8, cap: usize) -> usize {
  _ := value
  _ := out
  _ := cap
  ret 0
}

fn main() -> i32 {
  println("{} {}", "hello", 42u64)
  ret 0
}
