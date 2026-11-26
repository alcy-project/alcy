# FFI, Unsafe, and Platform Boundaries

The `unsafe` opt-in, `extern "C"` declarations, and the freestanding
link mode are Bootstrap, described here as they work today. The rest
of the chapter is reserved: it is kept so the design does not
foreclose the baremetal story.

## Unsafe (Bootstrap)

- The opt-in is `unsafe fn` and `unsafe { ... }` blocks, and it is
  lexical: an operation it covers may appear only inside such a
  block, and an `unsafe fn` body is not one implicitly, so a reader
  sees which act is unsafe rather than merely that the function is.
- `unsafe { ... }` is an expression whose value is the block's, and
  everything inside is checked as it was outside: the opt-in changes
  what is allowed, never what is inferred, borrowed, or moved.
- A call to an unsafe function is an operation the opt-in covers. An
  unsafe function that becomes a value waits for unsafe function
  types, so using one in value position is refused rather than
  handing safe code a call with no opt-in on it.
- Dereferencing a raw pointer is an operation the opt-in covers, for a
  read and for a write. Writing needs a `*mut` pointer and a `mut`
  binding that names it, the same way writing through a reference
  needs a `&mut` and a `mut` binding.
- Offsetting a raw pointer is an operation the opt-in covers. It is a
  call to `ptr_offset`/`ptr_offset_mut` (`items.md`), not a rule of
  `*`, so the opt-in sits on the call.
- The precondition-carrying intrinsics are declared `unsafe` —
  `memcopy`, `str_from_parts`, `slice_from_parts`,
  `slice_from_parts_mut`, `alloc`, `dealloc`, `elem_ptr`, `elem_ref`,
  `uninit_assume`, `ptr_offset`, and `ptr_offset_mut` — and the
  checker verifies the marker against the canonical set the way it
  verifies the shape (`items.md`), so the declaration and the opt-in
  cannot drift. `uninit_write` and `uninit_ref` stay safe: they are
  the safe half of `MaybeUninit`. Byte access, `size_of`, `align_of`,
  `print`, `println`, `panic`, and `sys_write` stay safe too.
- Safe code MUST NOT be able to cause undefined behavior; that
  guarantee stays load bearing. Unsafe code carries the obligations
  the compiler cannot check: a dereference names a live, aligned,
  initialized object; `&mut` exclusivity holds through raw pointers
  as through references; an `extern "C"` call satisfies the callee's
  C contract; `uninit_assume` reads storage that was written. A safe
  abstraction built over unsafe operations is where those
  obligations are established.
- Unsafe receiver and spec methods are not implemented yet: the
  marker parses and is refused where it appears.

## Raw pointers (Bootstrap)

- `*T` and `*mut T` are thin, Copy, and outside the region system: a
  value that carries one carries no loan, and a loan the cast was
  taken through expires at the cast. They are the address without
  the obligation.
- Creating one is safe: `&x as *T`, `&mut x as *mut T`, `p as *mut
  U`, and `&x as usize` all move the address around without reading
  through it. `*mut T` coerces to `*T` the way `&mut T` coerces to
  `&T`, and `as` crosses raw pointer kinds, pointees, and integers in
  both directions, which is what makes `0 as *T` the null pointer.
  `&T` never casts to a `*mut T`, and a raw pointer never casts back
  to a reference: a tracked reference has to be built where its loan
  comes from.
- Dereferencing and offsetting are operations the opt-in covers. A
  `*mut T` names a writable place; a `*T` reads only, and a write
  through one is rejected before the opt-in is consulted.
- Two raw pointers of one type compare with `==` and `!=` by
  address; ordering is not defined. An erased `void*` will spell as
  `*u8` and cast until it earns its own spelling.

## External functions (Bootstrap)

- `extern "C" { ... }` declares bodyless functions, and each
  declared name is the symbol the linker resolves: an extern
  declaration is not mangled. `"C"` is the only convention accepted;
  another string is a diagnostic. The block is one item for
  visibility, so `pub extern "C"` exports every declaration in it.
- Calling a declared function is an operation the opt-in covers, so
  the call names `unsafe { ... }` and an extern function is refused
  in value position until unsafe function types land. There is no
  `unsafe extern` spelling.
- The initial ABI is scalars, raw pointers, and `()` as a return. A
  `char` maps to an unsigned byte and a `rune` to a 32-bit word.
  Everything else is refused with a diagnostic: by-value aggregates,
  `str`/slices/references, closures, generics, and variadics. The
  callee's own contract is the caller's `unsafe` block to keep.
- The staged `ffi` package supplies `CStr` and `CString` for the bytes
  an extern declaration reads through a pointer. Their termination,
  borrowing, and UTF-8 decoding contracts are in `types.md`.

## Freestanding (Bootstrap)

- The default executable link runs in-process with the linker the
  compiler carries, on Linux, macOS, and Windows; a `linker` in
  `.alcy/toolchain.toml` or `--linker` on the command line replaces
  it, and an install whose build embedded no linker falls back to
  spawning a driver from PATH. A build targeting another machine
  needs a configured driver: the embedded linker links what the host
  can run, and nothing else.
- A hosted build for a Linux target links the system C runtime by
  default: the C runtime's startup enters `main`, which keeps its
  signature, and an `i32` main is the process status, `()` is zero,
  and an enum main writes `main returned Err` to stderr and exits 1
  on a variant other than the first. A hosted build goes through the
  C runtime's startup objects, so a sanitizer runtime can join it;
  a freestanding one cannot, which is the reason the sanitizers run
  against the hosted build. The embedded linker links this default
  too, probing the host's usual startup files and loader once per
  process. `.alcy/toolchain.toml` with `freestanding = true` asks
  for the kernel-only link instead: no startup objects, no dynamic
  linker, and no libc. On that link the kernel enters `_start`,
  which reads `argc` and `argv` off the initial stack into the
  registers the program's `main` expects and calls it; the program
  ends through the exit syscall, with the same rules for the exit
  code.
- A hosted build for a macOS target links the SDK's C runtime
  startup object and `libSystem`, and nothing else. libSystem is the
  one sanctioned C library on Apple platforms: no glibc or musl, no
  libc++, and no other library. The startup is the SDK's
  `usr/lib/crt1.o` and the library is the SDK's `usr/lib/libSystem.tbd`;
  both are discovered from `SDKROOT`, then `DEVELOPER_DIR` (an Xcode
  Developer directory, or any root with an `SDKs` directory), then the
  usual SDK locations (Command Line Tools, then Xcode), highest
  version first. The link fixes the deployment floor to macOS 11.0,
  which arm64 requires and which bounds the load commands and the
  symbol variants `libSystem.tbd` carries. A host with no SDK falls
  back to the spawned driver; a `--time-trace` session names the link
  `lld-link` when the embedded linker ran, so the two paths are
  distinguishable in a log.
- A hosted build for a Windows target links `kernel32.dll` and
  nothing else: no startup objects, no C runtime, no UCRT. The entry
  point is `_start`, which the loader enters with no arguments; it
  gathers them from `GetCommandLineW`, parsed the way a command line
  is documented to parse, before the program starts. The kernel is the
  allocator (`VirtualAlloc`), the standard handles are the descriptors,
  and the exit is `ExitProcess`, so an `i32` main is the process
  status and an enum main behaves as it does everywhere else. The link
  needs only the kernel import library: it is discovered from
  `WindowsSdkDir` and `WindowsSDKVersion`, then the newest `10.0.*`
  kit under `Windows Kits\10\Lib`, and a host with no SDK falls back
  to the spawned driver.
- `.alcy/toolchain.toml` with `freestanding = true` asks for the
  kernel-only link. On Linux the hosted default links the system C
  runtime, so the flag is how a Linux build asks for it; the entry
  is `_start` exactly as above, with the same rules, on a package
  that opted in. A Windows target has
  no separate freestanding mode: the kernel-only link is what a
  hosted build is. A macOS target has no freestanding mode either:
  libSystem is the only sanctioned route to the kernel there, so
  `freestanding = true` is refused with a diagnostic naming the
  target rather than linking a kernel-only program the runtime
  cannot serve.
- A runtime piece is defined only when the program declares it, so a
  program carries only what it uses. The pieces reach the kernel
  through raw syscalls: `print`, `println`, and `panic`
  write with the write syscall, `panic` then exits nonzero, and
  `alloc`/`dealloc` map and unmap anonymous memory, so the heap
  containers work without a C allocator. The sequences are written
  for x86-64, aarch64, and riscv64.

## Layout and statics

- Default struct layout is compiler-chosen. `#[repr(C)]` on a `struct`
  item fixes the struct's layout to the platform's C layout: fields in
  declaration order, each at the next offset its own alignment allows,
  with tail padding to the struct's own alignment, and nothing else.
  The default layout Alcy gives structs already satisfies the rule;
  the attribute is a contract with FFI, not a different layout, so the
  shape stays put whatever the compiler-chosen default becomes, and a
  C declaration and the Alcy `struct` it names keep sharing offsets
  across compiler versions and platforms (subject to each field's own
  C alignment on the target). The attribute applies to structs only
  and refuses a struct with no fields, mirroring C, which has no empty
  struct. `#[when]`, the item gate on the platform axis, poses no
  interaction with `repr(C)`: both may appear, one is a build decision
  and the other a shape. C fixed-width integer types are intended to be
  bit-compatible with Alcy integer types.
- `static` items MUST NOT contain `&mut`. Shared references to static
  storage are permitted. Mutable statics do not exist, and adding
  them needs careful review.

## Mangling

- Alcy-convention symbols are mangled per signature and start `_A`;
  `extern "C"` names are unmangled (see `modules.md`).
