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

fn pick(comp n: i32) -> i32 {
  ret match n {
    x if x > 5 => 100,
    x => 1,
  }
}

// A pattern condition binds inside the comp engine the way the
// runtime lowering does.
fn choose(comp o: Option<i32>) -> i32 {
  ret (if Option::Some(v) := o { v } else { 0 })
}

fn drain(comp first: Option<i32>) -> i32 {
  mut cur := first
  mut n := 0
  while Option::Some(v) := cur {
    n = n + v
    break
  }
  ret n
}

enum Choice { Num(i32), None }

// A comp argument of an enum type materializes its payload in the slot.
fn unwrap_choice(comp c: Choice) -> i32 {
  ret match c {
    Choice::Num(v) => v,
    Choice::None => 0,
  }
}

// A bare literal is re-checked against its sibling's type, so the comp
// engine must read the same last entry the lowering does; the first
// entry would truncate `x` to the literal's initial width.
fn narrow(comp x: i64) -> bool {
  ret 42 < x
}

fn main() -> i32 {
  const k := 21
  // A comp-evaluated call: `narrow`'s body runs in the comp engine,
  // where `42 < x` must read the literal's re-checked width.
  const narrow_ok := narrow(4294967296i64)
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
  // A guard decides the arm in comp evaluation too.
  if pick(3) != 1 {
    ret 13
  }
  if pick(7) != 100 {
    ret 14
  }
  if choose(Option::Some(4)) != 4 {
    ret 15
  }
  if drain(Option::Some(5)) != 5 {
    ret 16
  }
  if unwrap_choice(Choice::Num(6)) != 6 {
    ret 17
  }
  // A unit variant is comp-known like any other constant.
  if choose(Option::None) != 0 {
    ret 18
  }
  if drain(Option::None) != 0 {
    ret 19
  }
  if !narrow_ok {
    ret 20
  }
  ret double(k) + sum_to(4) - comp { 1 + 2 }
}
