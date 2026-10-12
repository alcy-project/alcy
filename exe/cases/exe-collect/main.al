// `collect`'s item type comes from the bound (`I: Iterator<T>`), so a
// call spells nothing: the implementation the argument's type has for
// `Iterator` answers `T`.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(3)
  v.push(1)
  v.push(2)

  by_ref := collect(v.iter())
  if *by_ref[0] != 3 || *by_ref[1] != 1 || *by_ref[2] != 2 {
    ret 1
  }
  println("{} {} {}", *by_ref[0], *by_ref[1], *by_ref[2])

  big: FilterIter<VecIter<i32>, &i32> :=
      FilterIter { iter: v.iter(), f: (x: &i32) -> *x > 1 }
  filtered := collect(big)
  if *filtered[0] != 3 || *filtered[1] != 2 {
    ret 2
  }
  println("{} {}", *filtered[0], *filtered[1])

  owned := collect(v.into_iter())
  if owned[0] != 3 || owned[2] != 2 {
    ret 3
  }
  println("{} {} {}", owned[0], owned[1], owned[2])

  strings := collect(["a", "b"].into_iter())
  println("{} {}", strings[0], strings[1])
  ret 0
}
