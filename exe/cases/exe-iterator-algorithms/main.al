// The free algorithms over any iterator: count, any, all, position,
// find, last, and collect_into.

fn is_even(x: i32) -> bool {
  ret x % 2 == 0
}

fn is_big(x: i32) -> bool {
  ret x > 2
}

fn main() -> i32 {
  mut v: Vec<i32> := Vec::<i32>::new()
  v.push(1)
  v.push(2)
  v.push(3)
  if count::<VecIter<i32>>(v.iter()) != 3 {
    ret 1
  }

  if !any::<RangeIter<i32>, i32>((1..=3).into_iter(), is_even) {
    ret 2
  }
  if all::<RangeIter<i32>, i32>((1..=3).into_iter(), is_big) {
    ret 3
  }
  match position::<RangeIter<i32>, i32>((1..=3).into_iter(), is_even) {
    Option::Some(at) => {
      if at != 1 {
        ret 4
      }
    }
    Option::None => {
      ret 5
    }
  }
  match find::<RangeIter<i32>, i32>((1..=3).into_iter(), is_big) {
    Option::Some(v) => {
      if v != 3 {
        ret 6
      }
    }
    Option::None => {
      ret 7
    }
  }
  match last::<RangeIter<i32>, i32>((1..=3).into_iter()) {
    Option::Some(v) => {
      if v != 3 {
        ret 8
      }
    }
    Option::None => {
      ret 9
    }
  }

  mut out: Vec<i32> := Vec::<i32>::new()
  collect_into::<RangeIter<i32>, i32>((1..=3).into_iter(), &mut out)
  if out.len() != 3 {
    ret 10
  }
  if *out.at(2).or_panic_with_message("expected a value") != 3 {
    ret 11
  }
  ret 0
}
