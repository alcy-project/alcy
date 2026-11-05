// An aggregate too large to live in a register crosses as a pointer to
// the caller's slot, and the callee works on its own copy (ADR-0139).
fn last(a: [u8; 70000]) -> u8 {
  ret a[69999]
}

fn clobber(mut a: [u8; 5000]) -> u8 {
  a[0] = 9
  ret a[0]
}

fn sum_pair(a: [u8; 5000], b: [u8; 5000]) -> i32 {
  ret (a[4999] as i32) + (b[0] as i32)
}

fn main() -> i32 {
  x := [3u8; 70000]
  print("{}\n", last(x))
  y := [4u8; 5000]
  z := [5u8; 5000]
  print("{}\n", clobber(y))
  print("{} {}\n", y[0], y[4999])
  print("{}\n", sum_pair(y, z))
  print("{} {}\n", x[0], x[69999])
  ret 0
}
