// A result too large to return in a register lands in a slot the caller
// provides, and reaches it whole (ADR-0139).
struct Big {
  buf: [u8; 5000],
  tag: i32,
}

fn make(fill: u8) -> [u8; 70000] {
  ret [fill; 70000]
}

fn build(tag: i32) -> Big {
  ret Big { buf: [tag as u8; 5000], tag: tag }
}

fn main() -> i32 {
  a := make(5)
  print("{} {}\n", a[0], a[69999])
  b := build(7)
  print("{} {}\n", b.tag, b.buf[4999])
  m := [[1u8; 70000]; 2]
  print("{} {}\n", m[0][69999], m[1][0])
  ret 0
}
