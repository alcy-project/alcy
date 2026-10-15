fn double(comp n: i32) -> i32 {
  ret n * 2
}

fn sum_to(comp n: i32) -> i32 {
  ret comp {
    mut t := 0
    mut i := 1
    while i <= n {
      t = t + i
      i = i + 1
    }
    t
  }
}

fn main() -> i32 {
  const k := 21
  // Short-circuit: the untaken side is never evaluated, even though the
  // comp engine would refuse the division inside it.
  const lhs_false := false && (1 / 0 == 0)
  const lhs_true := true || (1 / 0 == 0)
  if lhs_false {
    ret 11
  }
  if !lhs_true {
    ret 12
  }
  ret double(k) + sum_to(4) - comp { 1 + 2 }
}
