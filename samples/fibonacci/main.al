// Iterative Fibonacci: two variables carry the pair, so each step reads
// the previous two rather than reaching back for them.
fn fib(n: i32) -> i32 {
  mut a := 0
  mut b := 1
  for _ in 0..<n {
    next := a + b
    a = b
    b = next
  }
  ret a
}

fn main() -> i32 {
  for n in 0..<10 {
    println("fib({}) = {}", n, fib(n))
  }
  ret 0
}
