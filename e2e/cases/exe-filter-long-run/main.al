// `FilterIter::next` pulls rejected values in a loop, so a long run of
// them costs no stack: a million rejections still leave the depth of
// the program alone.
fn main() -> i32 {
  mut v := Vec::<u8>::new()
  mut i := 0usize
  while i < 1000000 {
    v.push(1u8)
    i = i + 1
  }
  println("built {}", v.len())
  hits :=
    count::<FilterIter<SliceIter<u8>, &u8>, &u8>(
      filter(v.as_slice().iter(), (x: &u8) -> *x == 200u8)
    )
  println("matched {}", hits)
  ret 0
}
