// The uninhabited type takes no room, so it may sit in a field, a
// tuple, an array, or a parameter, and the layout gives each shape a
// size without a value ever existing.
struct Dead {
  a: !,
  b: u8,
}

enum Maybe {
  A(!),
  B,
}

fn boom() -> ! {
  panic("boom")
}

fn takes(x: !) -> i32 {
  ret 3
}

fn main() -> i32 {
  if size_of::<Dead>() != 1 { ret 1 }
  if size_of::<(u8, !)>() != 1 { ret 2 }
  if size_of::<[!; 4]>() != 0 { ret 3 }
  if size_of::<Maybe>() != 4 { ret 4 }
  if size_of::<()>() != 0 { ret 5 }
  if align_of::<Dead>() != 1 { ret 6 }
  if size_of::<(!, u64)>() != 8 { ret 7 }
  ret 0
}
