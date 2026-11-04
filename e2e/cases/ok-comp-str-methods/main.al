// A `str` method reads its receiver through a reference and a comparison
// orders two strings by their bytes; comp evaluation reads both, so a
// comp block splices the value the runtime produces.

comp fn ordered(a: str, b: str) -> bool {
  ret a < b
}

fn main() -> i32 {
  if comp { "aébc".len_bytes() } != 5usize {
    ret 1
  }
  if !comp { "".is_empty() } {
    ret 2
  }
  if comp { "abc".byte(1) } != 98u8 {
    ret 3
  }
  if comp { "abcdef".byte_slice(1, 3) } != "bc" {
    ret 4
  }
  if !comp { "abc" < "abd" } {
    ret 5
  }
  if !comp { "ab" < "abc" } {
    ret 6
  }
  if comp { "abd" <= "abc" } {
    ret 7
  }
  if !comp { ordered("abc", "abd") } {
    ret 8
  }
  println("comp str ok")
  ret 0
}
