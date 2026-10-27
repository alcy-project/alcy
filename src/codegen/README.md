# codegen

The backend seam: `Backend`, `EmitRequest`, `EmitOptions`, and the
`Target` a build emits for. `codegen_llvm` and `codegen/wasm` are its
two implementations, and one dispatch file (`pipeline/backend_emit.cc`)
names them, so nothing outside the seam includes a backend's headers.

A new implementation meets the same contract before it wires in:
consume verified IR, return `base::Result`, report through the bag
(`docs/adr/0049-the-direct-backends-and-the-playground.md`).
