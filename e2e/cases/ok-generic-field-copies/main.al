// Resolving one field of a generic struct can instantiate another
// generic nominal: `Leaf<i32>` is first created while `Pair`'s first
// field resolves, and `Leaf<u8>` while the second does. The field-type
// copies form one contiguous range, so they must be appended only after
// every field has resolved.
struct Leaf<T> {
  slot: &mut MaybeUninit<T>
}

struct Pair<S, T> {
  first: Leaf<S>,
  second: Leaf<T>
}

fn touch(p: &Pair<i32, u8>) -> i32 {
  ret 0
}

fn main() -> i32 {
  ret 0
}
