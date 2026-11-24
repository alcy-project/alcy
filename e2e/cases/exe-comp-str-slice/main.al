// str_slice with compile-time bounds is evaluated by the comp emitter;
// an integer literal is a bare immutable, not a semantic value.
fn slice(comp s: str, comp a: usize, comp b: usize) -> str {
  ret comp { str_slice(s, a, b) }
}

fn main() -> i32 {
  const t: str := slice("abcdef", 1usize, 3usize)
  if str_len(t) != 2usize {
    ret 1
  }
  if t != "bc" {
    ret 1
  }
  ret 0
}
