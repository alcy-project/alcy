#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builds and serves the site.

The site under `site/` holds the landing page and the playground page as
templates plus the shared shell: TypeScript and plain HTML and CSS that
is not self-contained. The sources compile to the JavaScript the pages
load, highlighting needs the grammar compiled to wasm and a tree-sitter
binding, and the Check and Run buttons need the compiler's wasm module.
The site's own generator (the docsite binary built from `src/ssg`)
stamps one page tree per language
(English at the root, `/ja/` beside it) from the templates and the guide
under `docs/guide/`, checking the guide's examples with the same compiler
module. This assembles all of those into `site/dist/`, which any static
file server can host.

    uv run ./tools/site.py build            # assemble site/dist
    uv run ./tools/site.py test             # run the generator's tests
    uv run ./tools/site.py budget           # check site/dist against its budgets
    uv run ./tools/site.py serve            # serve it on :8000

`build` builds the docsite generator first through ninja, which keeps
it in step with the compiler and the standard library the guide's
fences use. An up-to-date build is a no-op. The generator is a host
release build, independent of the build's `--mode` and `--build-subdir`.

`build` takes whatever compiler artifacts it finds and warns when there
are none, so the site still works for highlighting alone. Pass
`--with-compiler` to build the `playground` target first; it carries the
direct wasm backend and no LLVM (see `src/playground/README.md`). That
module only serves the playground's Check and Run buttons; the guide's
fences are compiled by the generator itself.

The TypeScript compiler and the tree-sitter web binding are installed
with bun into `out/site-cache/`, at the versions `config.toml` pins;
`bun` and `tree-sitter` must be on `PATH`.
"""

import argparse
import gzip
import hashlib
import http.server
import io
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tomllib
from dataclasses import dataclass
from functools import partial
from pathlib import Path

from utils.paths import project_root_dir

from build import build

SITE_DIR = project_root_dir / "site"
PLAYGROUND_DIR = SITE_DIR / "playground"
SHARED_DIR = SITE_DIR / "shared"
DEFAULT_DIST_DIR = SITE_DIR / "dist"
# The user-facing guide; the site's generator renders it.
GUIDE_DIR = project_root_dir / "docs" / "guide"
SPEC_DIR = project_root_dir / "docs" / "spec"
TOOLS_DIR = project_root_dir / "docs" / "tools"
# Where the playground page lives inside the assembled site.
PAGE_SUBDIR = "playground"

# The languages the generator emits a page tree for; English is
# the tree at the site root and every other language lives under its
# own prefix.
SITE_LANGUAGES = ("en", "ja")
DEFAULT_LANGUAGE = "en"
# The JavaScript the site loads, as emitted by `tsc`.
BUILD_DIR = SITE_DIR / "build"
GRAMMAR_DIR = project_root_dir / "treesitter"
DEFAULT_BUILD_SUBDIR = "playground"
DEFAULT_WASM_TARGET = "playground"
# The generator is a separate host release build: it links the compiler
# for the fence check, so it does not share the playground's target OS,
# mode, or build directory.
DOCSITE_TARGET = "docsite"
DOCSITE_BUILD_SUBDIR = "docsite"
# The site's builds carry no LLVM. The playground needs no backends, and
# the native generator needs the host C++ library: the pinned libc++ that
# a normal compiler build expects is not installed in the site job.
DEFAULT_GN_ARG = "alcy_backends=[] alcy_use_libcxx=false"
CACHE_DIR = project_root_dir / "out" / "site-cache"
DEPS_DIR = CACHE_DIR / "deps"

# The page's modules, compiled from `site/playground/*.ts` into `build/`.
# `types` carries only types and emits nothing the page loads, so it is
# not here.
TYPESCRIPT_MODULES = [
    "app",
    "assets",
    "compiler.worker",
    "editor",
    "elements",
    "highlight",
    "output",
    "problems",
    "runner.worker",
    "runtime",
    "samples",
    "session",
    "splitter",
    "state",
    "status",
    "tabs",
    "textutil",
    "wasm-api",
    "wasi",
]

PLAIN_FILES = ["style.css"]

# The shared shell: the plain files are copied as they are, and everything
# under `build/shared/` was compiled from `site/shared/*.ts`.
SHARED_PLAIN_FILES = ["guide.css", "site.css"]


@dataclass(frozen=True)
class Versions:
    bun: str
    typescript: str
    web_tree_sitter: str


def log(message: str) -> None:
    print(f"site: {message}")


def fail(message: str) -> int:
    print(f"site: error: {message}", file=sys.stderr)
    return 1


# The site's weight budgets. Every page loads the shell's CSS and module;
# a guide page with a runnable program adds `guide.js`, which imports
# `guide-run.js`, and only the playground ever loads the compiler. Bytes
# are what the server sends (gzip for text, as GitHub Pages and
# `site.py serve` both do), because that is what a reader pays for; the
# per-asset ceilings leave room for ordinary growth and none for a module
# that quietly pulls in the playground's runtime.
@dataclass(frozen=True)
class Budget:
    path: str
    bytes: int
    note: str


PAGE_BUDGETS = (
    Budget("shared/site.css", 6_000, "the shell's stylesheet"),
    Budget("shared/site.js", 3_000, "theme, prefetch, the service worker"),
    Budget("shared/shell.js", 1_200, "every guide page's own module"),
    Budget("shared/i18n.en.js", 2_600, "the English catalog every page loads"),
    Budget("shared/i18n.ja.js", 3_100, "the Japanese catalog, loaded by its tree"),
    Budget("playground/app.js", 4_000, "the playground's wiring"),
    Budget("playground/style.css", 6_000, "the playground's layout"),
)

# A guide page with code pays for its blocks' Copy button, and a page
# with a runnable program pays for Run too. The ceiling covers every
# script a runnable page names or fetches on its own initiative.
GUIDE_SCRIPT_BUDGET = 8_000

# The copy-only half rides along with every code page, so it is the half
# that has to stay small.
GUIDE_COPY_BUDGET = 3_000

# The editor's half is fetched on the first Edit rather than with the
# page, so it has a ceiling of its own: it is the one download a reader
# can decline by not pressing the button.
GUIDE_EDIT_BUDGET = 3_000

# The heaviest files the playground can fetch on demand: `tree-sitter-alcy`
# for the editor's colors, and the binding that loads it. A page that is only
# read fetches neither. The grammar is the one asset a build can skip, so
# it has a name the budget walk can leave out when it was not built.
GRAMMAR_BUDGET = Budget(
    "playground/grammar/tree-sitter-alcy.wasm", 64_000, "the grammar"
)
ASSET_BUDGETS = (
    GRAMMAR_BUDGET,
    Budget("playground/vendor/web-tree-sitter.js", 40_000, "the binding"),
)

# The compiler module is a ceiling, not a ratchet. Its size belongs to the
# compiler: it grows with the language, and a ratchet would make every compiler
# feature trip a job that has nothing to say about it. What the site owes the
# reader is that the module stays in its size class, so the number sits where
# the module would have to change by a large factor to cross it. It catches an
# LLVM-linked module in a build that carries no LLVM, a debug module shipped by
# mistake, and a dependency that arrives with more than the compiler. The
# reader pays for it once, on the first Check or Run, and the worker keeps it.
COMPILER_MODULE_CEILING = Budget(
    "playground/compiler/alcy_playground.wasm",
    1_000_000,
    "the compiler module, compressed",
)


# The compression every server applies to the site's text and wasm, so a
# budget names the bytes a reader downloads rather than the bytes on disk.
TEXT_SUFFIXES = {".css", ".html", ".js", ".json", ".svg", ".txt", ".wasm", ".xml"}


def measured(path: Path) -> int:
    if not TEXT_SUFFIXES & {path.suffix}:
        return path.stat().st_size
    with open(path, "rb") as handle:
        return len(gzip.compress(handle.read(), 9))


@dataclass(frozen=True)
class Weighing:
    path: str
    size: int
    budget: int


def weigh(dist_dir: Path, budget: Budget, errors: list[str]) -> Weighing | None:
    path = dist_dir / budget.path
    if not path.is_file():
        errors.append(f"{budget.path}: missing ({budget.note})")
        return None
    size = measured(path)
    if size > budget.bytes:
        errors.append(
            f"{budget.path}: {size} bytes is over the {budget.bytes} budget "
            f"({budget.note})"
        )
    return Weighing(budget.path, size, budget.bytes)


def weigh_guide_scripts(dist_dir: Path, errors: list[str]) -> list[Weighing]:
    """Holds both halves of the guide's script to their budgets."""
    rows: list[Weighing] = []
    for path, budget, note in (
        ("guide-copy.js", GUIDE_COPY_BUDGET, "every code page's buttons"),
        ("guide-run.js", GUIDE_SCRIPT_BUDGET, "a runnable block's Run"),
        ("guide-edit.js", GUIDE_EDIT_BUDGET, "the editor, fetched on first Edit"),
    ):
        compressed = measured(dist_dir / "shared" / path)
        if compressed > budget:
            errors.append(
                f"shared/{path}: {compressed} bytes is over the {budget} "
                f"budget ({note})"
            )
        rows.append(Weighing(f"shared/{path}", compressed, budget))
    return rows


def measure_budgets(
    dist_dir: Path, errors: list[str], skip_grammar: bool = False
) -> list[Weighing]:
    """Measures every budget, recording the ones past their number."""
    assets = (
        tuple(budget for budget in ASSET_BUDGETS if budget != GRAMMAR_BUDGET)
        if skip_grammar
        else ASSET_BUDGETS
    )
    weighed = (
        weigh(dist_dir, budget, errors)
        for budget in PAGE_BUDGETS + assets + (COMPILER_MODULE_CEILING,)
    )
    rows = [row for row in weighed if row is not None]
    return rows + weigh_guide_scripts(dist_dir, errors)


def log_weights(rows: list[Weighing], stream: io.TextIOBase | None = None) -> None:
    destination = sys.stdout if stream is None else stream
    for row in rows:
        print(f"site: budget: {row.path} {row.size} / {row.budget}", file=destination)


def budget_markdown(rows: list[Weighing]) -> str:
    """The weights as the table a step summary renders."""
    lines = ["### Site weight", "", "| Asset | Size | Budget |", "| :-- | --: | --: |"]
    for row in rows:
        lines.append(f"| `{row.path}` | {row.size:,} | {row.budget:,} |")
    return "\n".join(lines)


def command_budget(args: argparse.Namespace) -> int:
    dist_dir = args.dist.resolve()
    if not (dist_dir / "index.html").is_file():
        return fail(f"{dist_dir} has no index.html; run `site.py build` first")
    errors: list[str] = []
    rows = measure_budgets(dist_dir, errors)
    if args.format == "markdown":
        # The table is what this command prints, so a workflow can send it
        # wherever the run keeps its summary; the log stays on stderr.
        log_weights(rows, sys.stderr)
        print(budget_markdown(rows))
    else:
        log_weights(rows)
    if errors:
        for message in errors:
            print(f"site: {message}", file=sys.stderr)
        return 1
    if args.format == "text":
        log("budgets held")
    return 0


def load_versions() -> Versions:
    with open(project_root_dir / "config.toml", "rb") as handle:
        config = tomllib.load(handle)
    return Versions(
        bun=config["bun_version"],
        typescript=config["typescript_version"],
        web_tree_sitter=config["web_tree_sitter_version"],
    )


def series(version: str) -> str:
    return ".".join(version.split(".")[:2])


def ensure_bun(pinned: str) -> str:
    bun = shutil.which("bun")
    if bun is None:
        raise RuntimeError(f"bun is not on PATH; install {pinned} (see site/README.md)")
    found = subprocess.run(
        [bun, "--version"], capture_output=True, text=True, check=True
    ).stdout.strip()
    if series(found) != series(pinned):
        raise RuntimeError(
            f"bun {found} is on PATH; this build pins {pinned} "
            "(config.toml). Install the pinned series."
        )
    return bun


def install_dependencies(versions: Versions) -> Path:
    bun = ensure_bun(versions.bun)
    DEPS_DIR.mkdir(parents=True, exist_ok=True)
    manifest = DEPS_DIR / "package.json"
    wanted = json.dumps(
        {
            "name": "alcy-site-deps",
            "private": True,
            "version": "0.0.0",
            "dependencies": {
                "typescript": versions.typescript,
                "web-tree-sitter": versions.web_tree_sitter,
            },
        },
        indent=2,
    )
    if not manifest.is_file() or manifest.read_text(encoding="utf-8") != wanted:
        manifest.write_text(wanted + "\n", encoding="utf-8")
    subprocess.run(
        [
            bun,
            "install",
            "--cwd",
            str(DEPS_DIR),
            # Nothing here needs a lifecycle script.
            "--ignore-scripts",
            "--silent",
        ],
        check=True,
    )
    modules = DEPS_DIR / "node_modules"
    log(
        f"deps: typescript {versions.typescript}, "
        f"web-tree-sitter {versions.web_tree_sitter}"
    )
    return modules


def generate_catalogs() -> None:
    if not (SITE_DIR / "shared" / "catalog.json").is_file():
        raise FileNotFoundError("site/shared/catalog.json is missing")
    result = subprocess.run(
        [sys.executable, str(project_root_dir / "tools" / "catalog.py")],
        check=False,
        cwd=project_root_dir,
    )
    if result.returncode != 0:
        raise RuntimeError("tools/catalog.py failed")


def build_typescript(modules: Path) -> None:
    bun = shutil.which("bun")
    if bun is None:
        raise RuntimeError("bun is not on PATH; needed to run tsc")
    tsc = modules / "typescript" / "lib" / "tsc.js"
    if not tsc.is_file():
        raise FileNotFoundError(f"{tsc} is missing")
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    for config in ("tsconfig.json", "tsconfig.workers.json"):
        subprocess.run(
            [bun, str(tsc), "-p", config],
            check=True,
            cwd=SITE_DIR,
        )
    log(f"typescript: {BUILD_DIR}")


def copy_static(dist_dir: Path) -> None:
    target = dist_dir / PAGE_SUBDIR
    target.mkdir(parents=True, exist_ok=True)
    for name in PLAIN_FILES:
        source = PLAYGROUND_DIR / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, target / name)
    for module in TYPESCRIPT_MODULES:
        source = BUILD_DIR / PAGE_SUBDIR / f"{module}.js"
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing; run the TypeScript build")
        shutil.copy2(source, target / source.name)
        source_map = source.parent / f"{module}.js.map"
        if source_map.is_file():
            shutil.copy2(source_map, target / source_map.name)


def copy_shared(dist_dir: Path) -> None:
    target = dist_dir / "shared"
    target.mkdir(parents=True, exist_ok=True)
    for name in SHARED_PLAIN_FILES:
        source = SHARED_DIR / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, target / name)
    for source in sorted((BUILD_DIR / "shared").glob("*.js")):
        if source.name == "sw.js":
            # The service worker serves from the site's root, not here:
            # its scope is its own directory and the whole site needs it.
            continue
        shutil.copy2(source, target / source.name)
        source_map = source.with_suffix(".js.map")
        if source_map.is_file():
            shutil.copy2(source_map, target / source_map.name)


# The catalog import the shared modules carry. One set of shared modules
# serves every tree, so the import names the catalog instead of a file and
# each page's import map says which file that name is. A page that carries
# no map runs its scripts with no catalog at all, and its toggle dies on
# the first label it tries to read.
SHARED_CATALOG_IMPORT = re.compile(
    r'^import \* as catalog from "([^"]+)";$', re.MULTILINE
)
IMPORT_MAP = re.compile(r'<script type="importmap">(.+?)</script>', re.DOTALL)
# An import map is only consulted for loads that happen after it, so it
# has to come before the first module the page pulls.
MODULE_LOAD = re.compile('type="module"|rel="modulepreload"')


def catalog_specifier(dist_dir: Path) -> str:
    """The name the shared modules import their catalog by."""
    source = (dist_dir / "shared" / "site.js").read_text(encoding="utf-8")
    found = SHARED_CATALOG_IMPORT.findall(source)
    if len(found) != 1:
        raise RuntimeError(
            "shared/site.js does not import exactly one catalog as catalog; "
            "the pages have no name to answer for"
        )
    specifier = found[0]
    if specifier.startswith((".", "/")):
        raise RuntimeError(
            f"shared/site.js imports the catalog by {specifier}, a path that "
            "cannot point at a tree; the import must be a bare specifier"
        )
    return specifier


def page_imports(relative: Path, body: str) -> dict[str, str]:
    """The pairs the import map a page carries states."""
    try:
        imports = json.loads(body)["imports"]
    except ValueError:
        raise RuntimeError(
            f"{relative} has an import map that does not parse"
        ) from None
    except KeyError:
        raise RuntimeError(f"{relative} has an import map with no imports") from None
    return imports


def expected_catalog(relative: Path) -> str:
    """The catalog module a page names, written from the page."""
    first = relative.parts[0] if len(relative.parts) > 1 else DEFAULT_LANGUAGE
    language = first if first in SITE_LANGUAGES else DEFAULT_LANGUAGE
    path = posixpath.relpath(
        f"shared/i18n.{language}.js", start=relative.parent.as_posix()
    )
    # An import map value must be resolvable against the document: a
    # root-tree path needs the "./" that the generator stamps too.
    return path if path.startswith(".") else f"./{path}"


def check_page_catalogs(dist_dir: Path) -> None:
    """Fails unless every page names the catalog of its own tree."""
    specifier = catalog_specifier(dist_dir)
    pages = sorted(dist_dir.rglob("*.html"))
    for page in pages:
        relative = page.relative_to(dist_dir)
        text = page.read_text(encoding="utf-8")
        maps = list(IMPORT_MAP.finditer(text))
        if len(maps) != 1:
            raise RuntimeError(f"{relative} carries {len(maps)} import maps, not one")
        load = MODULE_LOAD.search(text)
        if load is not None and load.start() < maps[0].start():
            raise RuntimeError(f"{relative} loads a module before its import map")
        imports = page_imports(relative, maps[0].group(1))
        expected = expected_catalog(relative)
        if imports.get(specifier) != expected:
            raise RuntimeError(
                f"{relative} names {imports.get(specifier)} as {specifier}, "
                f"not {expected}, the catalog of its tree"
            )
        if not (page.parent / expected).is_file():
            raise RuntimeError(f"{relative} names a catalog module that is missing")
    log(f"catalogs: {len(pages)} pages name the module of their tree")


# The service worker must sit at the site's root for its scope.
def copy_service_worker(dist_dir: Path) -> None:
    source = BUILD_DIR / "shared" / "sw.js"
    if not source.is_file():
        raise FileNotFoundError(f"{source} is missing; run the TypeScript build")
    shutil.copy2(source, dist_dir / "sw.js")
    source_map = source.with_suffix(".js.map")
    if source_map.is_file():
        shutil.copy2(source_map, dist_dir / "sw.js.map")


# The assets the service worker keeps warm; the version file beside its
# script names the cache from exactly these, so a deploy that changes
# any of them retires the cache on the next page load.
ASSET_PATHS = (
    "playground/compiler/alcy_playground.js",
    "playground/compiler/alcy_playground.wasm",
    "playground/vendor/web-tree-sitter.js",
    "playground/vendor/web-tree-sitter.wasm",
    "playground/grammar/tree-sitter-alcy.wasm",
)


def asset_version(dist_dir: Path) -> str:
    digest = hashlib.sha256()
    for part in ASSET_PATHS:
        path = dist_dir / part
        if not path.is_file():
            continue
        digest.update(path.name.encode("utf-8"))
        with open(path, "rb") as handle:
            for block in iter(lambda: handle.read(1 << 16), b""):
                digest.update(block)
    return digest.hexdigest()[:16]


def write_asset_version(dist_dir: Path) -> str:
    version = asset_version(dist_dir)
    (dist_dir / "asset-version.json").write_text(
        json.dumps({"v": version}) + "\n", encoding="utf-8"
    )
    log(f"asset cache: {version}")
    return version


# The service worker reads its cache's name from a string literal the
# build fills in, so the installed worker knows the version without a
# request; `asset-version.json` stays as the out-of-band copy it
# re-reads to notice a deploy. Only the literal is substituted: the
# name also appears in the worker's own declarations.
SW_VERSION_DECLARATION = 'const __ALCY_ASSET_VERSION__ = "__ALCY_ASSET_VERSION__";'


def stamp_service_worker(dist_dir: Path, version: str) -> None:
    script = dist_dir / "sw.js"
    text = script.read_text(encoding="utf-8")
    declaration = SW_VERSION_DECLARATION.replace('"__ALCY_ASSET_VERSION__"', '""')
    if declaration not in text:
        raise RuntimeError(f"{script} has no version declaration to stamp")
    script.write_text(
        text.replace(declaration, declaration.replace('""', f'"{version}"')),
        encoding="utf-8",
    )
    log(f"service worker: version {version}")


def copy_samples(dist_dir: Path) -> None:
    samples_dir = PLAYGROUND_DIR / "samples"
    manifest_path = samples_dir / "samples.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"{manifest_path} is missing")
    with open(manifest_path, "rb") as handle:
        manifest = json.loads(handle.read().decode("utf-8"))
    samples = manifest["samples"]
    target = dist_dir / PAGE_SUBDIR / "samples"
    target.mkdir(parents=True, exist_ok=True)
    # The manifest the page reads carries no repository paths; a sample
    # sourced from the repository's own `samples/` suite is copied under
    # its site name like the local ones.
    site_manifest = {
        "default": manifest.get("default"),
        "samples": [
            {key: value for key, value in sample.items() if key != "source"}
            for sample in samples
        ],
    }
    (target / "index.json").write_text(
        json.dumps(site_manifest, indent=2) + "\n", encoding="utf-8"
    )
    for sample in samples:
        source = (
            project_root_dir / sample["source"]
            if "source" in sample
            else samples_dir / sample["file"]
        )
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, target / sample["file"])


def build_grammar(dist_dir: Path) -> None:
    grammar_out = dist_dir / PAGE_SUBDIR / "grammar"
    grammar_out.mkdir(parents=True, exist_ok=True)
    highlights = GRAMMAR_DIR / "queries" / "alcy" / "highlights.scm"
    shutil.copy2(highlights, grammar_out / "highlights.scm")
    tree_sitter = shutil.which("tree-sitter")
    if tree_sitter is None:
        raise RuntimeError(
            "tree-sitter is not on PATH; install it (see CONTRIBUTING.md) "
            "or pass --skip-grammar"
        )
    output = grammar_out / "tree-sitter-alcy.wasm"
    subprocess.run(
        [tree_sitter, "build", "--wasm", "-o", str(output), str(GRAMMAR_DIR)],
        check=True,
        cwd=project_root_dir,
    )
    log(f"grammar: {output}")


def copy_web_tree_sitter(modules: Path, dist_dir: Path, version: str) -> None:
    vendor_out = dist_dir / PAGE_SUBDIR / "vendor"
    vendor_out.mkdir(parents=True, exist_ok=True)
    sources = modules / "web-tree-sitter"
    for name in ("web-tree-sitter.js", "web-tree-sitter.wasm"):
        source = sources / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, vendor_out / name)
    log(f"vendor: web-tree-sitter {version}")


def find_compiler_dir(build_dir: Path) -> Path | None:
    if (build_dir / "alcy_playground.js").is_file():
        return build_dir
    matches = sorted(build_dir.rglob("alcy_playground.js"))
    return matches[0].parent if matches else None


def copy_compiler(dist_dir: Path, build_dir: Path, compiler_dir: Path | None) -> bool:
    source = compiler_dir if compiler_dir is not None else find_compiler_dir(build_dir)
    if source is None:
        log(
            f"compiler: no alcy_playground.js under {build_dir}; "
            "Check and Run will be unavailable (build it with --with-compiler)"
        )
        return False
    glue = source / "alcy_playground.js"
    binary = source / "alcy_playground.wasm"
    if not glue.is_file() or not binary.is_file():
        raise FileNotFoundError(
            "both alcy_playground.js and alcy_playground.wasm are needed; "
            f"{source} has one"
        )
    target = dist_dir / PAGE_SUBDIR / "compiler"
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(glue, target / "alcy_playground.js")
    shutil.copy2(binary, target / "alcy_playground.wasm")
    log(f"compiler: {glue} ({binary.stat().st_size} bytes of wasm)")
    return True


# The provenance every page's footer shows: the tag when the built
# commit carries one (a release redeploy), the short sha otherwise. CI
# passes a release build's tag through `ALCY_BUILD_STAMP`; everything
# else asks git, and a build outside a repository says so.
REPO_URL = "https://github.com/alcy-project/alcy"
STAMP_SHA_LENGTH = 9


def git_text(repo_root: Path, arguments: list[str]) -> str:
    result = subprocess.run(
        ["git", *arguments], cwd=repo_root, capture_output=True, text=True
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def build_stamp(repo_root: Path) -> tuple[str, str]:
    tag = os.environ.get("ALCY_BUILD_STAMP", "").strip()
    if tag == "":
        tag = git_text(repo_root, ["describe", "--exact-match", "--tags", "HEAD"])
    if tag != "":
        return tag, f"{REPO_URL}/tree/{tag}"
    sha = git_text(repo_root, ["rev-parse", f"--short={STAMP_SHA_LENGTH}", "HEAD"])
    if sha != "":
        return sha, f"{REPO_URL}/commit/{sha}"
    return "unknown", ""


def build_guide(dist_dir: Path) -> None:
    # alcy-docsite stamps every page tree, verifies every fence and
    # walks the link audit natively. It links the compiler for the
    # fence check, so its build is a release build.
    docsite = ensure_docsite()
    stamp, stamp_url = build_stamp(project_root_dir)
    command = [
        str(docsite),
        "--docs",
        str(GUIDE_DIR),
        "--spec",
        str(SPEC_DIR),
        "--tools",
        str(TOOLS_DIR),
        "--dist",
        str(dist_dir),
        "--site",
        str(SITE_DIR),
        "--repo-root",
        str(project_root_dir),
        "--stamp",
        stamp,
        "--stamp-url",
        stamp_url,
    ]
    subprocess.run(command, check=True, cwd=project_root_dir)


def ensure_docsite() -> Path:
    # Always go through ninja, even when the binary is on disk: it links
    # the compiler and links the standard library into the fence check,
    # so a stale binary compiles the guide against an older language.
    # An up-to-date build is a no-op.
    candidate = project_root_dir / "out" / DOCSITE_BUILD_SUBDIR / "alcy-docsite"
    log("building the docsite generator")
    code = build(
        target=DOCSITE_TARGET,
        mode="release",
        build_subdir=DOCSITE_BUILD_SUBDIR,
        gn_args_extra=DEFAULT_GN_ARG,
    )
    if code != 0:
        raise RuntimeError(f"docsite build failed ({code})")
    return candidate


def command_build(args: argparse.Namespace) -> int:
    if args.with_compiler:
        code = build(
            target=args.with_compiler,
            mode=args.mode,
            build_subdir=args.build_subdir,
            target_os="emscripten",
            gn_args_extra=DEFAULT_GN_ARG if args.gn_arg is None else args.gn_arg,
        )
        if code != 0:
            return code

    versions = load_versions()
    modules = install_dependencies(versions)
    generate_catalogs()
    build_typescript(modules)

    dist_dir = args.dist.resolve()
    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    dist_dir.mkdir(parents=True)

    copy_static(dist_dir)
    copy_shared(dist_dir)
    copy_service_worker(dist_dir)
    copy_samples(dist_dir)

    if not args.skip_grammar:
        build_grammar(dist_dir)
    copy_web_tree_sitter(modules, dist_dir, versions.web_tree_sitter)
    copy_compiler(
        dist_dir, project_root_dir / "out" / args.build_subdir, args.compiler_dir
    )
    stamp_service_worker(dist_dir, write_asset_version(dist_dir))
    build_guide(dist_dir)
    check_page_catalogs(dist_dir)

    errors: list[str] = []
    log_weights(measure_budgets(dist_dir, errors, skip_grammar=args.skip_grammar))
    if errors:
        for message in errors:
            print(f"site: {message}", file=sys.stderr)
        return 1

    log(f"site ready: {dist_dir}")
    log(f"serve it with: uv run ./tools/site.py serve --dist {dist_dir}")
    return 0


def command_test(_args: argparse.Namespace) -> int:
    # The generator tests are the ssg sources of the compiler test suite.
    # Build and run them in the no-LLVM configuration the generator itself
    # uses: the site job installs no C++ toolchain, and the normal build
    # needs pinned libc++ headers that only setup-build provides.
    # emsdk puts its own clang++ first on PATH, which targets wasm and
    # cannot find the host C++ headers the native generator needs. Drop
    # those entries for this native build; the wasm build above keeps
    # them.
    env = dict(os.environ)
    path = [
        entry
        for entry in env.get("PATH", "").split(os.pathsep)
        if "emsdk" not in entry and "emscripten" not in entry
    ]
    env["PATH"] = os.pathsep.join(path)
    code = build(
        target="tests",
        mode="release",
        build_subdir=DOCSITE_BUILD_SUBDIR,
        gn_args_extra=DEFAULT_GN_ARG,
        env=env,
    )
    if code != 0:
        return fail(f"tests build failed ({code})")
    result = subprocess.run(
        [
            sys.executable,
            str(project_root_dir / "tools" / "run.py"),
            "--target=tests",
            "--mode=release",
            "--build-subdir=" + DOCSITE_BUILD_SUBDIR,
            "--no-build",
            "--",
            "--source-file=*src/tests/ssg/*",
        ],
        check=False,
        cwd=project_root_dir,
    )
    if result.returncode != 0:
        return fail("tests failed")
    log("tests passed")
    return 0


def command_serve(args: argparse.Namespace) -> int:
    dist_dir = args.dist.resolve()
    if not (dist_dir / "index.html").is_file():
        return fail(f"{dist_dir} has no index.html; run `site.py build` first")

    # GitHub Pages compresses the site's text assets; the local server does
    # the same so what is measured here matches what is deployed. The
    # grammar wasm compresses to about an eighth of its size.
    compressible = {
        ".al",
        ".css",
        ".html",
        ".js",
        ".json",
        ".map",
        ".mjs",
        ".scm",
        ".svg",
        ".txt",
        ".wasm",
        ".xml",
    }

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {
            **http.server.SimpleHTTPRequestHandler.extensions_map,
            ".al": "text/plain; charset=utf-8",
            ".mjs": "text/javascript; charset=utf-8",
            ".scm": "text/plain; charset=utf-8",
            ".wasm": "application/wasm",
        }

        def end_headers(self) -> None:
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def send_head(self):
            path = self.translate_path(self.path)
            # GitHub Pages answers a missing path with `404.html` under
            # the requested URL; the local server does the same, so a
            # broken link reads here the way it will in production.
            not_found = dist_dir / "404.html"
            if not os.path.exists(path) and not_found.is_file():
                body = not_found.read_bytes()
                self.send_response(404)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                return io.BytesIO(body)
            if (
                not os.path.isdir(path)
                and os.path.splitext(path)[1] in compressible
                and "gzip" in self.headers.get("Accept-Encoding", "")
            ):
                try:
                    with open(path, "rb") as handle:
                        body = handle.read()
                except OSError:
                    self.send_error(404, "File not found")
                    return None
                compressed = gzip.compress(body, 6)
                self.send_response(200)
                self.send_header("Content-Type", self.guess_type(path))
                self.send_header("Content-Encoding", "gzip")
                self.send_header("Vary", "Accept-Encoding")
                self.send_header("Content-Length", str(len(compressed)))
                self.end_headers()
                return io.BytesIO(compressed)
            return super().send_head()

    handler = partial(Handler, directory=str(dist_dir))
    server = http.server.ThreadingHTTPServer((args.host, args.port), handler)
    log(f"serving {dist_dir} at http://{args.host}:{server.server_port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and serve the site.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_parser = subparsers.add_parser("build", help="assemble site/dist")
    build_parser.add_argument("--mode", choices=["debug", "release"], default="release")
    build_parser.add_argument("--build-subdir", default=DEFAULT_BUILD_SUBDIR)
    build_parser.add_argument("--dist", type=Path, default=DEFAULT_DIST_DIR)
    build_parser.add_argument("--compiler-dir", type=Path, default=None)
    build_parser.add_argument(
        "--with-compiler",
        nargs="?",
        const=DEFAULT_WASM_TARGET,
        default=None,
        metavar="TARGET",
        help=f"Build the compiler wasm first (default target: {DEFAULT_WASM_TARGET})",
    )
    build_parser.add_argument(
        "--gn-arg",
        default=None,
        help="GN arguments for --with-compiler, passed through verbatim "
        f'(default: "{DEFAULT_GN_ARG}")',
    )
    build_parser.add_argument("--skip-grammar", action="store_true")
    build_parser.set_defaults(handler=command_build)

    test_parser = subparsers.add_parser("test", help="run the generator's tests")
    test_parser.set_defaults(handler=command_test)

    budget_parser = subparsers.add_parser(
        "budget", help="check the assembled site against its weight budgets"
    )
    budget_parser.add_argument("--dist", type=Path, default=DEFAULT_DIST_DIR)
    budget_parser.add_argument(
        "--format",
        choices=("text", "markdown"),
        default="text",
        help="text logs the weights; markdown prints the table for a summary",
    )
    budget_parser.set_defaults(handler=command_budget)

    serve_parser = subparsers.add_parser("serve", help="serve the assembled site")
    serve_parser.add_argument("--dist", type=Path, default=DEFAULT_DIST_DIR)
    serve_parser.add_argument("--host", default="0.0.0.0")
    serve_parser.add_argument("--port", type=int, default=8000)
    serve_parser.set_defaults(handler=command_serve)

    args = parser.parse_args()
    try:
        return args.handler(args)
    except (FileNotFoundError, RuntimeError, subprocess.CalledProcessError) as error:
        return fail(str(error))


if __name__ == "__main__":
    sys.exit(main())
