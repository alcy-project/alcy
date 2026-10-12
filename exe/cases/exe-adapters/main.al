// The adapters' constructors: the element type comes from the
// `I: Iterator<T>` bound, so a call spells only the predicate or the
// count.

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  v.push(4)

  big := collect(filter(v.iter(), (x: &i32) -> *x > 1))
  if *big[0] != 2 || *big[2] != 4 {
    ret 1
  }
  println("{} {} {}", *big[0], *big[1], *big[2])

  first := collect(take(v.iter(), 2))
  if *first[0] != 1 || *first[1] != 2 {
    ret 2
  }
  println("{} {}", *first[0], *first[1])

  rest := collect(skip(v.iter(), 3))
  if *rest[0] != 4 {
    ret 3
  }
  println("{}", *rest[0])

  numbered := collect(enumerate(v.iter()))
  if numbered[2].0 != 2 || *numbered[2].1 != 3 {
    ret 4
  }
  println("{} {}", numbered[2].0, *numbered[2].1)
  ret 0
}
