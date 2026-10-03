fn main() -> i32 {
  // The wildcard takes a slot without a name: `a` is the second
  // argument, and binding it to the first is the bug this pins down.
  g := (_: i32, a: i32) -> a
  if g(1, 2) != 2 { ret 1 }
  h := (mut a: i32, _: i32) -> a
  if h(3, 4) != 3 { ret 2 }
  ret 0
}
