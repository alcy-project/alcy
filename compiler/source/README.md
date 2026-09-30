# source

File loading and identity for the compiler pipeline.

`SourceManager` loads files, dedups them by canonical path, and hands
out stable `FileId` values. Loaded bytes and display names stay owned
by the manager for the whole compilation; every later stage refers to
source text through `FileId`, never by path.

## Entry points

- `SourceManager::load(path)` -> `base::Result<FileId, SourceError>`.
  `SourceError::OpenFailed` when the file cannot be read.
- `SourceManager::bytes(id)` / `name(id)` ->
  `std::optional<std::string_view>`. `std::nullopt` names no loaded
  file; an engaged empty view is a loaded empty file. The two cases
  are distinct: unknown ids must be rejected, empty files accepted.

## Input requirements

- Callers must handle `std::nullopt` from `bytes`/`name`: an unknown
  `FileId` is caller error and is reported (analyzer:
  `ANALYZER_INVALID_PATH`) rather than read as empty.
- `FileId` values are only meaningful for the manager that issued
  them; they are indices, not handles, and must not cross managers.
