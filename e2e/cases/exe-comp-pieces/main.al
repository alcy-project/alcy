use core::string::str_len;
use fmt::display::comp_pieces;

comp fn check(comp s: str) -> usize {
  mut n := 0usize
  comp for p in comp_pieces(s) {
    if p.is_arg {
      n += p.precision + p.arg + 1
      if p.explicit_float {
        n += 100
      }
    } else {
      n += str_len(p.literal)
    }
  }
  ret n
}

fn main() -> i32 {
  const n := check("a{:.3f}b")
  println("{}", n)
  if n != 106 {
    ret 1
  }
  ret 0
}
