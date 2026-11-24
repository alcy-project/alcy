// A fixed one-byte discriminant must be read and written as a byte.
#[layout(tag_size = 1)]
enum Flag {
  Off,
  On,
}

fn pick(f: Flag) -> i32 {
  match f {
    Flag::Off => ret 1,
    Flag::On => ret 2,
  }
}

fn main() -> i32 {
  if size_of::<Flag>() != 1 {
    ret 1
  }
  if pick(Flag::Off) != 1 {
    ret 2
  }
  if pick(Flag::On) != 2 {
    ret 3
  }
  ret 0
}
