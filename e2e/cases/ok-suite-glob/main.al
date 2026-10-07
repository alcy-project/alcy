use cli::run::go;
use fmt::show::shout;

fn main() -> i32 {
  ret go() + shout(1)
}
