// A slice is a pointer and a length travelling together. The point is
// the ABI at run time: the pair arrives whole across a call, and a
// write through an exclusive view lands in the caller's buffer.
fn sum(v: &[i32]) -> i32 {
  mut total := 0i32
  mut i := 0 as usize
  while i < slice_len(v) {
    total = total + v[i]
    i = i + 1
  }
  ret total
}

fn fill(mut v: &mut [i32], value: i32) {
  mut i := 0 as usize
  while i < slice_len(v) {
    v[i] = value
    i = i + 1
  }
}

fn main() -> i32 {
  mut a := [3i32, 1i32, 4i32, 1i32, 5i32]
  if sum(&a) != 14 {
    ret 1
  }
  fill(&mut a, 7i32)
  mut i := 0 as usize
  while i < 5 {
    if a[i] != 7 {
      ret 2
    }
    i = i + 1
  }
  // The view of a heap buffer survives the call that reads it whole.
  mut v := Vec::<i32>::new()
  mut j := 0i32
  while j < 1000 {
    v.push(j)
    j = j + 1
  }
  if sum(v.as_slice()) != 499500 {
    ret 3
  }
  // An empty buffer views as an empty slice, not a broken one.
  empty := Vec::<i32>::new()
  if slice_len(empty.as_slice()) != 0 {
    ret 4
  }
  mut t := String::new()
  t.push_str("abc")
  b := t.as_bytes()
  if slice_len(b) != 3 {
    ret 5
  }
  if b[2] != 99u8 {
    ret 6
  }
  ret 0
}
