// Each call in this chain holds a 70 KiB array by value, so the three
// frames sum past the reserve a fixed 64 KiB layout could hold. The
// reserve covers the frames the program declares, so the chain runs.
fn deepest(c: [u8; 70000]) -> i32 {
  ret c[69999] as i32
}

fn middle(b: [u8; 70000]) -> i32 {
  ret deepest(b) + (b[0] as i32)
}

fn outer(a: [u8; 70000]) -> i32 {
  ret middle(a) + (a[0] as i32)
}

fn main() -> i32 {
  x := [7u8; 70000]
  print("{}\n", outer(x))
  ret 0
}
