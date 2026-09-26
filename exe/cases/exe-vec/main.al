// `Vec<T>` is the first core type that hands out a reference into its
// own buffer, and the first to return an `Option<T>` a caller keeps
// across another call. Both used to be wrong: the accessor returned a
// borrowed pointer the borrow checker could not track, and the returned
// enum read a dead stack frame. See docs/adr/0012 and
// docs/adr/0014.
fn sum_all(mut v: Vec<i32>) -> i32 {
  mut total := 0i32
  mut i := 0 as usize
  while i < v.len() {
    total = total + *v.at_mut(i).unwrap()
    i = i + 1
  }
  ret total
}

fn takes_by_value(v: Vec<i32>) -> usize {
  ret v.len()
}

fn main() -> i32 {
  mut v := Vec::<i32>::new()
  if !v.is_empty() {
    ret 1
  }
  // Enough pushes to force several reallocations.
  mut i := 0i32
  while i < 1000 {
    v.push(i)
    i = i + 1
  }
  if v.len() != 1000 {
    ret 2
  }
  if sum_all(v) != 499500 {
    ret 3
  }
  // The vector survived a by-value call, so it is still whole.
  if v.len() != 1000 {
    ret 4
  }
  // A retained accessor result, consumed after other calls have run.
  // `Option<&mut T>` carries an exclusive reference, so it is move-only
  // and this is its one use.
  kept := v.at_mut(10 as usize)
  if takes_by_value(Vec::<i32>::new()) != 0 {
    ret 5
  }
  if *kept.unwrap() != 10i32 {
    ret 6
  }
  // Bounds: the last index is present, one past it is not.
  if v.at_mut(1000 as usize).is_some() {
    ret 7
  }
  if v.at_mut(999 as usize).is_none() {
    ret 8
  }
  last := v.pop()
  if last.is_none() {
    ret 9
  }
  if last.unwrap() != 999i32 {
    ret 10
  }
  if v.len() != 999 {
    ret 11
  }
  v.clear()
  if !v.is_empty() {
    ret 12
  }
  // Capacity survives a clear, and a reserved vector starts empty.
  if v.capacity() == 0 {
    ret 13
  }
  mut w := Vec::<i32>::with_capacity(8 as usize)
  if !w.is_empty() {
    ret 14
  }
  if w.capacity() != 8 {
    ret 15
  }
  if w.pop().is_some() {
    ret 16
  }
  ret 0
}
