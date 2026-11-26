// A string local in a comp block keeps its pool-backed value through
// the synthetic IR's Store/Load round trip.
const N: usize := comp {
  s := "abc"
  str_len(s)
}

fn main() -> i32 {
  if N != 3usize {
    ret 1
  }
  ret 0
}
