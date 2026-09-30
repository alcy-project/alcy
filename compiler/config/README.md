# config

Build-time configuration (`build_config.h`).

Compile-time flags describing the target (`IS_OS_*`,
`IS_DEBUG`, ...). Consumed through `BUILD_FLAG(...)`; never probed
at runtime for decisions the build already made.

## Input requirements

- Target properties (pointer width, OS) are passed explicitly to
  the stages that need them (`ir::PointerWidth`, `exe_suffix()`);
  `config` answers "what was this binary built as", never "what
  should this compilation do".
