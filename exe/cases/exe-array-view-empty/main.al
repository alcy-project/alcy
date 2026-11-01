// An array's own views need a base pointer, and an array of no
// elements still starts somewhere. The base is the array's address,
// read without the index check that element 0 would run.

fn sum<N: usize>(a: [u8; N]) -> usize {
  mut n := 0 as usize
  for b in a.iter() {
    x := *b
    n = n + (x as usize)
  }
  ret n
}

fn main() -> i32 {
  println("non empty {}", sum::<3>([1u8; 3]))
  empty := [0u8; 0]
  println("empty slice len {}", empty.as_slice().len())
  mut blank := [0u8; 0]
  println("empty mut slice len {}", blank.as_mut_slice().len())
  mut seen := 0
  for b in blank.iter_mut() {
    *b = 1u8
    seen = seen + 1
  }
  println("iterated {}", seen)
  ret 0
}
