// A size is an offset a pointer can carry: an array is its
// element's size times its length, and 2^80 bytes wraps to 0 in
// the layout a target could address.

fn main() -> i32 {
  println("{}", size_of::<[[[[u8; 1048576]; 1048576]; 1048576]; 1048576]>())
  ret 0
}
