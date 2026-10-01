use self::texts::Word;
use self::texts::Show;
use self::texts::Loud;

fn main() -> i32 {
  w := Word { s: "hi" }
  s := w.show()
  ret str_len(s) as i32
}
