// Two instantiations of one generic function that share a signature
// still compile apart: the type argument appears only in the body.

fn width<T>() -> i32 {
  ret size_of::<T>() as i32
}

fn main() -> i32 {
  if width::<i32>() != 4 {
    ret 1
  }
  if width::<u64>() != 8 {
    ret 2
  }
  if width::<u8>() != 1 {
    ret 3
  }
  ret 0
}
