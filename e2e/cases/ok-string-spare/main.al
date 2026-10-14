// A caller fills the spare capacity and records the length.
fn main() -> i32 {
  mut s := String::with_capacity(8)
  {
    b := s.spare_capacity_mut()
    if b.len() < 8 { ret 1 }
    b[0] = 0x41u8
    b[1] = 0x42u8
  }
  s.set_len(2)
  if s.as_str() != "AB" { ret 2 }
  if s.len() != 2 { ret 3 }
  s.push(0x43u8)
  if s.as_str() != "ABC" { ret 4 }
  ret 0
}
