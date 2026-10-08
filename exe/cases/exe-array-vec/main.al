// A value parameter's count is part of the instance: the storage has
// that many elements, writes stay inside it, and distinct counts are
// distinct types (ADR-0054).

struct ArrayVec<T, N: usize> {
  data: [T; N],
  len: usize
}

fn main() -> i32 {
  mut v: ArrayVec<i32, 4> := ArrayVec { data: [0, 0, 0, 0], len: 0 }
  v.data[0] = 10
  v.data[3] = 7
  v.len = 2
  if v.len != 2 {
    ret 1
  }
  if v.data[0] != 10 {
    ret 2
  }
  if v.data[3] != 7 {
    ret 3
  }

  w: ArrayVec<i32, 4> := v
  if w.data[0] != 10 {
    ret 4
  }

  z: ArrayVec<u8, 8> := ArrayVec {
    data: [1 as u8, 2 as u8, 3 as u8, 4 as u8, 5 as u8, 6 as u8, 7 as u8, 8 as u8],
    len: 8,
  }
  if z.data[7] != 8 as u8 {
    ret 5
  }
  ret 0
}
