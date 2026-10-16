// A module table: each module owns its symbol map, and a lookup walks
// the table by name.
struct Module {
  name: String,
  symbols: Map<i32>,
}

fn lookup(modules: &Vec<Module>, module_name: str, name: str) -> Option<i32> {
  mut i := 0 as usize
  while i < modules.len() {
    m := modules.at(i).unwrap()
    if m.name.as_str() == module_name {
      match m.symbols.get(name) {
        Option::Some(v) => {
          ret Option::Some(*v)
        }
        Option::None => {
          ret Option::None
        }
      }
    }
    i = i + 1
  }
  ret Option::None
}

fn main() -> i32 {
  mut root := Module { name: String::from("root"), symbols: Map::<i32>::new() }
  _ := root.symbols.insert("x", 1)
  _ := root.symbols.insert("y", 2)
  mut modules := Vec::<Module>::new()
  modules.push(root)
  modules.push(Module { name: String::from("lib"), symbols: Map::<i32>::new() })
  match lookup(&modules, "root", "x") {
    Option::Some(v) => {
      if v != 1 {
        ret 1
      }
    }
    Option::None => {
      ret 2
    }
  }
  match lookup(&modules, "lib", "x") {
    Option::Some(_) => {
      ret 3
    }
    Option::None => {}
  }
  match lookup(&modules, "nope", "x") {
    Option::Some(_) => {
      ret 4
    }
    Option::None => {}
  }
  ret 0
}
