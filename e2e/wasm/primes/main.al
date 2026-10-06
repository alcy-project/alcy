// The primes below 50, by trial division. Every composite has a factor
// at or below its square root, so the inner loop can stop there rather
// than trying every divisor up to n.

fn is_prime(n: i32) -> bool {
  if n < 2 {
    ret false
  }
  mut d := 2
  while d * d <= n {
    if n % d == 0 {
      ret false
    }
    d = d + 1
  }
  ret true
}

fn main() -> i32 {
  mut n := 2
  while n < 50 {
    if is_prime(n) {
      t := (n,)
      line := format("{}\n", t)
      print(line.as_str())
    }
    n = n + 1
  }
  ret 0
}
