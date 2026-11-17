// Compile-time evaluation runs the block's control flow: a while with
// break and continue, a match, and array indexing all answer before the
// program runs.

comp fn sum(comp n: i32) -> i32 {
  ret comp {
    mut total := 0
    mut i := 0
    while i < n {
      i = i + 1
      if i == 2 {
        continue
      }
      if i == 4 {
        break
      }
      total = total + i
    }
    total
  }
}

comp fn pick(comp i: i32) -> i32 {
  ret comp {
    a := [10, 20, 30]
    a[i]
  }
}

comp fn classify(comp n: i32) -> i32 {
  ret comp {
    match n {
      1 => 10,
      2 => 20,
      _ => 30,
    }
  }
}

fn main() -> i32 {
  const a := sum(10)
  const b := pick(1)
  const c := classify(2)
  const d := pick(2)
  if a != 4 {
    ret 1
  }
  if b != 20 {
    ret 2
  }
  if c != 20 {
    ret 3
  }
  if d != 30 {
    ret 4
  }
  ret 0
}
