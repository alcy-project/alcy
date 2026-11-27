// Layout settings must reach the comp evaluator too: a const query reads
// the same table as a runtime query, and a comp fn field access or enum
// match on an annotated type must agree with the lowered program.
#[layout(min_align = 16, max_align = 16)]
struct Wide {
  x: u8,
}

#[layout(tag_size = 1)]
enum Flag {
  Off,
  On,
}

struct Offset {
  a: u8,
  #[layout(max_align = 1)] tail: [u16; 0],
  c: u8,
}

const W: usize := size_of::<Wide>()
const A: usize := align_of::<Wide>()
const F: usize := size_of::<Flag>()

comp fn last(comp s: Offset) -> u8 {
  ret comp { s.c }
}

comp fn tag(comp f: Flag) -> i32 {
  ret comp {
    match f {
      Flag::Off => 1,
      Flag::On => 2,
    }
  }
}

fn main() -> i32 {
  if W != 16usize {
    ret 1
  }
  if A != 16usize {
    ret 2
  }
  if F != 1usize {
    ret 3
  }
  const v := last(Offset { a: 1, tail: [0u16; 0], c: 2 })
  if v != 2u8 {
    ret 4
  }
  const t := tag(Flag::On)
  if t != 2 {
    ret 5
  }
  ret 0
}
