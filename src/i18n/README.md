# i18n

The message catalog: the languages the compiler reports in, and the
text of every message it can print.

- `Language` is the set of languages, and `LANGUAGE_TAGS` names each one
  by the tag `--lang` takes. Tags are lowercase kebab-case and match
  exactly. Nothing reads the environment: `LANG` and `LC_ALL` belong to
  the shell, and a compiler whose output language depends on them is a
  different compiler on every machine.
- `Key` is one identity per message, named by the message rather than by
  the diagnostic code it is reported under, because several registered
  codes carry more than one wording. The ordinal is an index into every
  catalog and means nothing outside this module, so the enum is as wide
  as the catalog needs it to be.
- `messages.def` holds the texts. It is a list, not a table: the key
  enum, every language's table, and the completeness check are all
  generated from it, so they cannot disagree.
- `format_to<K>` composes a message and checks the format string of
  *every* catalog against the call site's arguments at compile time. A
  missing translation is a build error.
- `text<K>` is for a message with no arguments. A message that grows a
  placeholder stops compiling there.
- `format<K>` returns the composed message as a `std::string`, for the
  cli, which renders text rather than appending it. `format_to<K>` takes
  any output iterator fmt can write into.

## Adding a message

1. Add the entry to `messages.def` under its module's section, named
   after what is wrong. The diagnostic code stays at the call site.
2. Emit it with `DiagBag::emit<Key::Name>(severity, code, args...)`.

Changing a text is step 1 alone. Renaming a key invalidates every
translation of it, so a name is not changed to tidy up.

## Adding a language

1. Add `Xx` to `Language` and its tag to `LANGUAGE_TAGS`.
2. Add `lang/xx_yy.def` with `ALCY_I18N_FOREACH_KEY_XX_YY`, listing
   every key of the canonical list in the same order.
3. Add the language to `ALCY_I18N_FOREACH_LANGUAGE`.

The build rejects a catalog that misses a key, reorders one, or names a
key that does not exist. There is no fallback: a language reaches
`--lang` only when its catalog is complete.

Not messages: the JSON document's field names and machine values, the
units a measurement is written in (`KiB`, `ms`), flag spellings, and the
diagnostics of the developer (`DLOG`, `DCHECK`, `UNREACHABLE`).

The data source is an in-tree X-macro while there is one language and no
external translators. The seam is `Key` and `format_to<K>`: moving the
texts to `lang/<tag>.toml` with a generator that emits the same tables
changes this module and no call site.
