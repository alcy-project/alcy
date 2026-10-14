// A qualified generic function is reached like any other item: the
// module that declares it has to open it with `pub`.
fn main() -> i32 {
  ret helper::id::<i32>(7) - 7
}
