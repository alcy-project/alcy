// The libc-free runtime: `println` writes through the write syscall
// and `Vec` allocates through mmap, so the whole program runs on the
// kernel alone.
fn main() -> i32 {
  println("hello, kernel")
  mut v := Vec::<i32>::new()
  v.push(7)
  v.push(35)
  ret v.len() as i32 + 5
}
