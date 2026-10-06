// Fibonacci, iteratively. Two variables carry the pair, so each step
// reads the previous two rather than reaching back for them.
fn fib(n: i32) -> i32 {
  mut a := 0
  mut b := 1
  mut i := 0
  while i < n {
    next := a + b
    a = b
    b = next
    i = i + 1
  }
  ret a
}

fn main() -> i32 {
  mut n := 0
  while n < 10 {
    t := (n, fib(n))
    line := format("fib({}) = {}\n", t)
    print(line.as_str())
    n = n + 1
  }
  ret 0
}
