// A view a std constructor composes from a borrowed array is a loan on
// the array: handing it back out of the frame that owns the array is
// refused, the way a direct borrow of the array is.

fn f() -> &[i32] {
  a := [7i32, 8i32, 9i32]
  ret a.as_slice()
}

fn main() -> i32 {
  s := f()
  print("{}", s[0])
  ret 0
}
