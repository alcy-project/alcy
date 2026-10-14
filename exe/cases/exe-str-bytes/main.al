// `bytes()` names the same walk `for b in s` desugars to.
fn main() -> i32 {
  mut n := 0
  mut sum := 0u64
  for b in "ab".bytes() {
    n = n + 1
    sum = sum + (b as u64)
  }
  if n != 2 { ret 1 }
  if sum != 195u64 { ret 2 }
  s := format("A")
  mut c := 0
  for b in s.bytes() { c = c + 1 }
  if c != 1 { ret 3 }
  ret 0
}
