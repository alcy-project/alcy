// The sieve from the bug report: the fill loop, the marking loops, and
// the answer's growth all bind in their bodies, and the heap allocations
// sit right where the frame used to land once the loops walked the
// shadow stack past the reserve. The count and the endpoints are what
// say the data survived.
fn sieve(n: u64) -> Vec<u64> {
  limit := n as usize
  mut is_prime := Vec::<bool>::with_capacity(limit)
  mut primes := Vec::<u64>::with_capacity(limit)
  for _ in 0..<n {
    is_prime.push(true)
  }
  for i in 2..<n {
    if !is_prime[i as usize] {
      continue
    }
    primes.push(i)
    mut j := i * i
    while j < n {
      is_prime[j as usize] = false
      j += i
    }
  }
  ret primes
}

fn main() -> i32 {
  primes := sieve(1600)
  if primes.len() != 251 {
    ret 1
  }
  if primes[0] != 2 {
    ret 2
  }
  if primes[1] != 3 {
    ret 3
  }
  if primes[2] != 5 {
    ret 4
  }
  if primes[250] != 1597 {
    ret 5
  }
  ret 0
}
