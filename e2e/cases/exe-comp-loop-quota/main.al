// `loop` stays on the evaluation quota, not the expansion cap.
comp fn count(n: i32) -> i32 {
  mut i := 0
  loop {
    if i >= n {
      ret i
    }
    i += 1
  }
}

fn main() -> i32 {
  const c := count(4100)
  if c != 4100 {
    ret 1
  }
  ret 0
}
