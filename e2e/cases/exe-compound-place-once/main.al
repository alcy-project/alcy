// A compound assignment evaluates its place once: the index that names
// the element runs a single time for the read and the write alike.

struct Cell {
  n: i32,
}

fn next(counter: &mut i32) -> usize {
  i := *counter
  *counter = *counter + 1
  ret i as usize
}

fn main() -> i32 {
  // The read and the write reach the same element.
  mut calls := 0
  mut v := [1, 2, 3]
  v[next(&mut calls)] += 10
  if calls != 1 {
    ret 1
  }
  if v[0] != 11 || v[1] != 2 || v[2] != 3 {
    ret 2
  }
  // A field of an element: the index still runs once.
  mut cell_calls := 0
  mut cells := [Cell { n: 1 }, Cell { n: 2 }]
  cells[next(&mut cell_calls)].n += 10
  if cell_calls != 1 {
    ret 3
  }
  if cells[0].n != 11 || cells[1].n != 2 {
    ret 4
  }
  // A mutable slice reaches the element the same way.
  mut slice_calls := 0
  mut backing := [1, 2, 3]
  sl := &mut backing[..]
  sl[next(&mut slice_calls)] += 10
  if slice_calls != 1 {
    ret 5
  }
  if backing[0] != 11 || backing[1] != 2 {
    ret 6
  }
  // The value is read before the place, and each runs once.
  mut both := 0
  mut w := [1, 2, 3]
  w[next(&mut both)] += next(&mut both) as i32
  if both != 2 {
    ret 7
  }
  if w[0] != 1 || w[1] != 2 || w[2] != 3 {
    ret 8
  }
  // Every operator the compound form spells keeps the meaning the
  // binary form gives it.
  mut x := 2
  x **= 3
  x <<= 2
  x -= 2
  x /= 3
  x %= 6
  x |= 8
  x &= 12
  x ^= 4
  x >>= 2
  if x != 2 {
    ret 9
  }
  mut neg := -8
  neg >>= 2
  if neg != -2 {
    ret 10
  }
  // A comp block folds a compound assignment too.
  const folded := comp {
    mut i := 1
    i += 2
    i *= 2
    i
  }
  if folded != 6 {
    ret 11
  }
  ret 0
}
