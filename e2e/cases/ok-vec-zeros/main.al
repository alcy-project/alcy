// `zeros` fills a vector through `memset`, safe for types whose
// all-zero bytes spell a valid value.

fn main() -> i32 {
  ints := Vec::<i32>::zeros(4)
  if ints.len() != 4 {
    ret 1
  }
  if ints[0] != 0 || ints[3] != 0 {
    ret 2
  }
  flags := Vec::<bool>::zeros(3)
  if flags[0] {
    ret 3
  }
  chars := Vec::<char>::zeros(2)
  if chars[1] != '\0' {
    ret 4
  }
  ret 0
}
