// Raw pointers end to end: creation is safe, and read, write, and
// offset are the operations the gate covers. The cast drops the
// loan, so the owner may be written while the pointer exists.
fn main() -> i32 {
  mut x := 1
  p := &x as *i32
  x = 5
  a := unsafe { *p }

  mut buf := [10, 20, 30]
  mut base := &mut buf[0] as *mut i32
  mut second := unsafe { ptr_offset_mut(base, 1) }
  unsafe { *second = 25 }
  c := buf[1]
  d := unsafe { *ptr_offset(base, 2) }

  q := 0 as *i32
  if q == 0 as *i32 {
    ret (a - 5) + (c - 25) + (d - 30)
  }
  ret 100
}
