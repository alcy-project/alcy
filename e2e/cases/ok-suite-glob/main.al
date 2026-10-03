use acme_cli::run::go;
use acme_fmt::show::shout;

fn main() -> i32 {
  ret go() + shout(1)
}
