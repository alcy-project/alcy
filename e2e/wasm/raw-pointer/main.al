// Raw pointers end to end under the direct-wasm backend: creating one is
// safe, and read, write, offset, struct projection, and array indexing
// are the operations the gate covers. The offset builtins lower to an
// element offset whose destination is a raw pointer, and a projection
// through one walks to the field, neither of which this backend used to
// accept: the first failed an internal check, the second asked for a
// projection through a byte.
struct P {
  x: i32,
  y: i32,
}

fn main() -> i32 {
  mut x := 1
  p := &x as *i32
  x = 5
  a := unsafe { *p }
  if a != 5 {
    ret 1
  }

  mut buf := [10, 20, 30]
  mut base := &mut buf[0] as *mut i32
  mut second := unsafe { ptr_offset_mut(base, 1) }
  unsafe { *second = 25 }
  c := buf[1]
  if c != 25 {
    ret 2
  }
  d := unsafe { *ptr_offset(base, 2) }
  if d != 30 {
    ret 3
  }

  p0 := P { x: 7, y: 9 }
  r := &p0 as *P
  fy := unsafe { (*r).y }
  if fy != 9 {
    ret 4
  }
  fx := unsafe { (*r).x }
  if fx != 7 {
    ret 5
  }

  arr := [40, 50, 60]
  ap := &arr as *[i32; 3]
  last := unsafe { (*ap)[2] }
  if last != 60 {
    ret 6
  }

  q := 0 as *i32
  if q == 0 as *i32 {
    ret 0
  }
  ret 100
}
