// A large aggregate that reaches a block from several predecessors is
// stored by each branch, and a call through a function value passes and
// returns it the same way a direct call does (ADR-0139).
struct Big {
  buf: [u8; 5000],
  tag: i32,
}

fn build(tag: i32) -> Big {
  ret Big { buf: [tag as u8; 5000], tag: tag }
}

fn apply(f: ([u8; 5000]) -> [u8; 5000], v: [u8; 5000]) -> [u8; 5000] {
  ret f(v)
}

fn main() -> i32 {
  s := if 1 > 0 {
    build(21)
  } else {
    build(22)
  }
  print("{}\n", s.tag)
  p := match s.tag {
    21 => build(31),
    _ => build(32),
  }
  print("{} {}\n", p.tag, p.buf[4999])
  g := (v: [u8; 5000]) -> {
    mut out := v
    out[4999] = 77
    out
  }
  x := [1u8; 5000]
  y := apply(g, x)
  print("{} {} {}\n", x[4999], y[4999], y[0])
  ret 0
}
