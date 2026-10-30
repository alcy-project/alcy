// The faithful descriptor write (ADR-0117): one write(2) answering
// the bytes it accepted, and the error a bad descriptor reports.

use sys::raw::write;

fn main() -> i32 {
  match write(Fd::stdout(), "hi\n") {
    Result::Ok(n) => {
      if n != 3 {
        ret 1
      }
    }
    Result::Err(_) => {
      ret 2
    }
  }
  match write(Fd::from_raw(-1), "x") {
    Result::Ok(_) => {
      ret 3
    }
    Result::Err(_) => {}
  }
  ret 0
}
