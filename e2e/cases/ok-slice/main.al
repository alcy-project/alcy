// `&[T]` generalizes `str` to any element: a view a function walks
// without owning the buffer. A fixed array decays into one at the
// call, and a `Vec` hands one out through its two accessors.
fn sum(v: &[i32]) -> i32 {
  mut total := 0i32
  mut i := 0 as usize
  while i < slice_len(v) {
    total = total + v[i]
    i = i + 1
  }
  ret total
}

fn bump(mut v: &mut [i32]) {
  v[0] = v[0] + 1
}

fn main() -> i32 {
  mut a := [1i32, 2i32, 3i32]
  if sum(&a) != 6 {
    ret 1
  }
  bump(&mut a)
  if a[0] != 2 {
    ret 2
  }
  mut v := Vec::<i32>::new()
  mut i := 0i32
  while i < 5 {
    v.push(i)
    i = i + 1
  }
  s := v.as_slice()
  if sum(s) != 10 {
    ret 3
  }
  // The view is dead before the push that would reallocate under it.
  n := slice_len(s)
  v.push(5i32)
  if n != 5 {
    ret 4
  }
  mut t := String::new()
  t.push_str("hi")
  b := t.as_bytes()
  if slice_len(b) != 2 {
    ret 5
  }
  if b[0] != 104u8 {
    ret 6
  }
  ret 0
}
