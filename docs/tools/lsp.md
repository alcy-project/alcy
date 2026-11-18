# `alcy lsp`

Speak the Language Server Protocol over standard input and output. `lsp`
is what an editor talks to; nothing else calls it. The server answers
until the client closes the channel.

## Usage

```console
alcy lsp
```

There is one mode and no flags: `alcy lsp` runs over the pair of
streams until the client asks for shutdown.

## What it answers

- Hover (layout and signature of the item the cursor is on)
- Go to definition, references
- Completion (checked names, keywords)
- Inlay hints (inferred types, sizes, offsets)
- Semantic tokens from the lexer's own trivia
- Folding ranges and document symbols
- Call hierarchy, type hierarchy
- Code actions: the linter's repairs for the diagnostics the server is showing
- Workspace symbols, workspace diagnostics
- Semantic tokens delta answers (live editing sessions)
- Code lenses (reference counts)

`alcy lsp` owns no analysis of its own: every answer reads the same
checked package the compiler produced for the buffers the editor has
open. If a refactor of the compiler lands correctly, the language
server answers identically after a reload.

## Configuration

The server reads no default configuration file today; behavior is
whatever the build shipped with. The editor provides the buffers
over the LSP channel.

## Messages and errors

| Kind | Meaning |
| --- | --- |
| Request returns a JSON-RPC error | the request was ill-formed or named something the server does not support |
| `window/logMessage` | informational; the server logs to the log channel, never to stdout |
| `window/showMessage` | information for the user, in the same language as diagnostics |

## Exit codes

| Code | Meaning |
| --- | --- |
| (none on its happy path) | the client closes stdin; the exit is then arbitrary and has no meaning |

## See also

- [`alcy check`](check.md) - the frontend `lsp` reads
- [`alcy lint`](lint.md) - the rules the server's code actions apply
- `alcy lsp --help` - the synopsis for this build
