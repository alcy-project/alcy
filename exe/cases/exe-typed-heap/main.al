// Exercises the typed heap primitives: `size_of`/`align_of` type
// queries, `alloc<T>`/`dealloc<T>` element-counted ownership, and
// `elem_ptr<T>` typed element offsets read and written through `*p`.
struct Pair<T> { first: T, second: T }

fn fill(data: &mut i32, count: usize) {
  mut i := 0 as usize
  while i < count {
    mut slot := elem_ptr(data, i)
    *slot = i as i32 * 3
    i = i + 1
  }
}

fn sum(data: &mut i32, count: usize) -> i32 {
  mut total := 0
  mut i := 0 as usize
  while i < count {
    slot := elem_ptr(data, i)
    total = total + *slot
    i = i + 1
  }
  ret total
}

fn main() -> i32 {
  mut bad := 0
  if size_of::<i32>() != 4 {
    bad = 1
  }
  if size_of::<u8>() != 1 {
    bad = 2
  }
  if align_of::<i32>() != 4 {
    bad = 3
  }
  // A struct element is measured whole, not as its fields.
  if size_of::<Pair<i32>>() < 8 {
    bad = 4
  }

  count := 4 as usize
  data := alloc::<i32>(count)
  fill(data, count)
  if sum(data, count) != 18 {
    bad = 5
  }
  // A second buffer of a different element type, to show the
  // instantiation keys are distinct.
  bytes := alloc::<u8>(8)
  mut k := 0 as usize
  while k < 8 {
    mut byte := elem_ptr(bytes, k)
    *byte = k as u8
    k = k + 1
  }
  mut byte_total := 0
  k = 0
  while k < 8 {
    byte := elem_ptr(bytes, k)
    byte_total = byte_total + *byte as i32
    k = k + 1
  }
  if byte_total != 28 {
    bad = 6
  }
  dealloc(bytes, 8)
  dealloc(data, count)
  ret bad
}
