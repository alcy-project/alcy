// The platform errno maps to a stable portable kind. An unmapped
// number is Other, not a raw kind that happens to look valid.
use sys::error::ErrorKind;
use sys::error::kind_of;

fn name(kind: ErrorKind) -> i32 {
  ret match kind {
    ErrorKind::Interrupted => { ret 1 }
    ErrorKind::WouldBlock => { ret 2 }
    ErrorKind::NotFound => { ret 3 }
    ErrorKind::PermissionDenied => { ret 4 }
    ErrorKind::BrokenPipe => { ret 5 }
    ErrorKind::InvalidInput => { ret 6 }
    ErrorKind::Other => { ret 0 }
  }
}

fn main() -> i32 {
  if name(kind_of(4)) != 1 { ret 1 }
  if name(kind_of(11)) != 2 { ret 2 }
  if name(kind_of(2)) != 3 { ret 3 }
  if name(kind_of(13)) != 4 { ret 4 }
  if name(kind_of(32)) != 5 { ret 5 }
  if name(kind_of(22)) != 6 { ret 6 }
  if name(kind_of(9999999)) != 0 { ret 7 }
  ret 0
}
