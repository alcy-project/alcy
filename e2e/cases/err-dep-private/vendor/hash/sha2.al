fn hidden() -> i32 {
  ret 0
}

pub fn shown() -> i32 {
  ret hidden() + 1
}
