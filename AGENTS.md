# Project Context

This workspace contains `alcy`, a high-performance programming language compiler written in C++.

# Reference Documentation

Prior to generating, refactoring, or reviewing code, strictly follow the specifications in:
- `ARCHITECTURE.md`: Pipeline architecture, intermediate representation design, LLVM integration, and repository structure.
- `CONTRIBUTING.md`: Workflow scripts (`build.py`, `lint.py`), tooling setup, formatting, and commit conventions.

# Code Quality & Refactoring Directives

- **Modern & Idiomatic C++20**:
  - Target C++20 features (e.g., `std::span`, `std::optional`, `std::variant`, concepts, designated initializers).
  - Enforce explicit ownership using value semantics, `std::unique_ptr`, or `std::shared_ptr`. Avoid manual memory management (`new`/`delete`).
  - Keep functions pure and side-effect-free where possible. Prefer `const` by default for variables, members, and methods.
- **Error Handling & Constraints**:
  - The project builds with `-fno-exceptions` and `-fno-rtti`. **Do not use `try`, `catch`, `throw`, `dynamic_cast`, or RTTI.**
  - Use explicit, zero-overhead error reporting abstractions (e.g., `std::optional`, custom `Result`/`Status` types, or diagnostic handlers) instead of exceptions.
  - Use assertions (`DCHECK()`) or diagnostic logging (`src/base/`, `src/debug/`) for internal compiler invariant failures.
- **Signal-to-Noise Ratio in Comments**:
  - **Avoid Redundant Comments**: Do not write comments that merely restate obvious C++ code (e.g., `// constructor`, `// push to vector`).
  - **Keep Value-Additive Comments**: Retain or add comments only when explaining non-trivial algorithms, invariants, complex LLVM IR lowering logic, or memory lifetime ownership details.
- **Self-Documenting Code**: Prefer expressive namespaces, functions, type aliases, and strong types over heavy block comments.

# Code Generation Directives

- Respect module boundaries inside `src/*` and adhere strictly to the linear pipeline flow defined in `ARCHITECTURE.md`.
- Ensure new files include the project license header and have corresponding target entries in their module's `BUILD.gn`.
- Code generation, comments, documentation, and commit messages must be written in English.
