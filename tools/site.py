#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builds and serves the site.

The site under `site/` holds the landing page and the playground page as
templates plus the shared shell: TypeScript and plain HTML and CSS that
is not self-contained. The sources compile to the JavaScript the pages
load, highlighting needs the grammar compiled to wasm and a tree-sitter
binding, and the Check and Run buttons need the compiler's wasm module.
The site's own generator (`site/ssg/`) stamps one page tree per language
(English at the root, `/ja/` beside it) from the templates and the guide
under `docs/guide/`, checking the guide's examples with the same compiler
module. This assembles all of those into `site/dist/`, which any static
file server can host.

    uv run ./tools/site.py build            # assemble site/dist
    uv run ./tools/site.py test             # run the generator's tests
    uv run ./tools/site.py budget           # check site/dist against its budgets
    uv run ./tools/site.py serve            # serve it on :8000

`build` takes whatever compiler artifacts it finds and warns when there
are none, so the site still works for highlighting alone. Pass
`--with-compiler` to build the `playground` target first; it carries the
direct wasm backend and no LLVM (see `src/playground/README.md`).

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
# Where the playground page lives inside the assembled site.
PAGE_SUBDIR = "playground"
# The JavaScript the site loads, as emitted by `tsc`.
BUILD_DIR = SITE_DIR / "build"
GRAMMAR_DIR = project_root_dir / "treesitter"
DEFAULT_BUILD_SUBDIR = "playground"
DEFAULT_WASM_TARGET = "playground"
# The playground build carries no LLVM; `src/backends.gni` selects the
# backends, and an empty list is what a browser needs.
DEFAULT_GN_ARG = "alcy_backends=[]"
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
# a guide page with code adds `guide.js`, and only the playground ever
# loads the compiler. Bytes are what the server sends (gzip for text, as
# GitHub Pages and `site.py serve` both do), because that is what a
# reader pays for; the per-asset ceilings leave room for ordinary growth
# and none for a module that quietly pulls in the playground's runtime.
@dataclass(frozen=True)
class Budget:
    path: str
    bytes: int
    note: str


PAGE_BUDGETS = (
    Budget("shared/site.css", 6_000, "the shell's stylesheet"),
    Budget("shared/site.js", 3_000, "theme, prefetch, the service worker"),
    Budget("shared/shell.js", 1_200, "every guide page's own module"),
    Budget("shared/i18n.en.js", 2_400, "the English catalog every page loads"),
    Budget("shared/i18n.ja.js", 2_900, "the Japanese catalog, loaded by its tree"),
    Budget("playground/app.js", 4_000, "the playground's wiring"),
    Budget("playground/style.css", 6_000, "the playground's layout"),
)

# A guide page with code pays for its blocks' Copy and Edit buttons. The
# ceiling covers the whole budget of scripts the page names.
GUIDE_SCRIPT_BUDGET = 8_000

# The heaviest files the playground can fetch, though only on demand:
# `tree-sitter-alcy` for the editor's colors and the compiler for Check
# and Run. The compiler's module is the one number on the site that
# matters most, so it is budgeted rather than watched.
ASSET_BUDGETS = (
    Budget(
        "playground/compiler/alcy_playground.wasm",
        620_000,
        "the compiler module, compressed",
    ),
    Budget("playground/grammar/tree-sitter-alcy.wasm", 64_000, "the grammar"),
    Budget("playground/vendor/web-tree-sitter.js", 40_000, "the binding"),
)


# The compression every server applies to the site's text and wasm, so a
# budget names the bytes a reader downloads rather than the bytes on disk.
TEXT_SUFFIXES = {".css", ".html", ".js", ".json", ".svg", ".txt", ".wasm", ".xml"}


def measured(path: Path) -> int:
    if not TEXT_SUFFIXES & {path.suffix}:
        return path.stat().st_size
    with open(path, "rb") as handle:
        return len(gzip.compress(handle.read(), 9))


def check_budget(dist_dir: Path, budget: Budget, errors: list[str]) -> None:
    path = dist_dir / budget.path
    if not path.is_file():
        errors.append(f"{budget.path}: missing ({budget.note})")
        return
    size = measured(path)
    log(f"budget: {budget.path} {size} / {budget.bytes}")
    if size > budget.bytes:
        errors.append(
            f"{budget.path}: {size} bytes is over the {budget.bytes} budget "
            f"({budget.note})"
        )


def check_guide_scripts(dist_dir: Path, errors: list[str]) -> None:
    compressed = measured(dist_dir / "shared" / "guide.js")
    log(f"budget: shared/guide.js {compressed} / {GUIDE_SCRIPT_BUDGET}")
    if compressed > GUIDE_SCRIPT_BUDGET:
        errors.append(
            f"shared/guide.js: {compressed} bytes is over the "
            f"{GUIDE_SCRIPT_BUDGET} budget (a guide page's buttons)"
        )
    # The copy-only half stays small enough to ride along with every code
    # page; the run half is what a reader who asks for it pays for.
    copy_only = dist_dir / "shared" / "guide-copy.js"
    if copy_only.is_file():
        compressed = measured(copy_only)
        log(f"budget: shared/guide-copy.js {compressed} / 3_000")
        if compressed > 3_000:
            errors.append(
                f"shared/guide-copy.js: {compressed} bytes is over the 3_000 "
                "budget (every guide page's buttons)"
            )


def command_budget(args: argparse.Namespace) -> int:
    dist_dir = args.dist.resolve()
    if not (dist_dir / "index.html").is_file():
        return fail(f"{dist_dir} has no index.html; run `site.py build` first")
    errors: list[str] = []
    for budget in PAGE_BUDGETS + ASSET_BUDGETS:
        check_budget(dist_dir, budget, errors)
    check_guide_scripts(dist_dir, errors)
    if errors:
        for message in errors:
            print(f"site: {message}", file=sys.stderr)
        return 1
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


def build_typescript(modules: Path) -> None:
    bun = shutil.which("bun")
    if bun is None:
        raise RuntimeError("bun is not on PATH; needed to run tsc")
    tsc = modules / "typescript" / "lib" / "tsc.js"
    if not tsc.is_file():
        raise FileNotFoundError(f"{tsc} is missing")
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    for config in ("tsconfig.json", "tsconfig.workers.json", "tsconfig.ssg.json"):
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
        if source.name == "i18n.js":
            continue
        shutil.copy2(source, target / source.name)
        source_map = source.with_suffix(".js.map")
        if source_map.is_file():
            shutil.copy2(source_map, target / source_map.name)


# The languages the generator emits a page tree for. A tree's script
# loads one language's strings, not the table of all of them: each tree
# gets its own copy of `shared/i18n.js` with the other catalog removed.
# The module keeps every exported name, so nothing else changes.
SITE_LANGUAGES = ("en", "ja")
DEFAULT_LANGUAGE = "en"

# The two catalogs are adjacent blocks that end at a line holding only
# `};`, and the table that selects one by the page's language is a single
# line between them and `toLanguage`.
CATALOGS_TABLE = "const CATALOGS = { en, ja };"


def prune_catalogs(source: str, language: str) -> str:
    """Returns `source` with every catalog but `language` removed."""
    pruned = source
    for name in SITE_LANGUAGES:
        if name == language:
            continue
        pattern = re.compile(
            r"^(?:export )?const %s = \{\n[\s\S]*?^\};\n" % name, re.MULTILINE
        )
        pruned, count = pattern.subn("", pruned)
        if count != 1:
            raise RuntimeError(
                f"shared/i18n.js has no {name} catalog to remove for {language}"
            )
    if pruned.count(CATALOGS_TABLE) != 1:
        raise RuntimeError("shared/i18n.js has no CATALOGS table to prune")
    return pruned.replace(CATALOGS_TABLE, f"const CATALOGS = {{ {language} }};")


def write_catalogs(dist_dir: Path) -> None:
    source = (BUILD_DIR / "shared" / "i18n.js").read_text(encoding="utf-8")
    target = dist_dir / "shared"
    target.mkdir(parents=True, exist_ok=True)
    for language in SITE_LANGUAGES:
        (target / f"i18n.{language}.js").write_text(
            prune_catalogs(source, language), encoding="utf-8"
        )
    log(f"catalogs: {', '.join(SITE_LANGUAGES)}")


# The catalog import the shared modules carry. The build rewrites it in
# the tree's copy, which is the thing the compiler cannot know: one
# source serves every tree, and only the stamper knows which tree a page
# belongs to. A playground page reaches the shell through its own entry,
# so both entries are rewritten.
CATALOG_IMPORT = 'from "./i18n.js"'
SHARED_CATALOG_IMPORT = 'from "../shared/i18n.js"'


def catalog_users(route: str) -> list[Path]:
    """The modules that import the catalog on behalf of `route`'s tree."""
    users = [Path("shared") / "site.js"]
    if route.startswith(f"{PAGE_SUBDIR}/") or route.rstrip("/") == PAGE_SUBDIR:
        users.append(Path(PAGE_SUBDIR) / "app.js")
    return users


def stamp_catalog(dist_dir: Path, route: str, language: str) -> None:
    """Points the tree's catalog imports at that language's module."""
    for relative in catalog_users(route):
        module = dist_dir / relative
        source = module.read_text(encoding="utf-8")
        needle = (
            CATALOG_IMPORT if relative.parts[0] == "shared" else SHARED_CATALOG_IMPORT
        )
        count = source.count(needle)
        if count == 0 or f"i18n.{language}.js" in source:
            continue
        if count != 1:
            raise RuntimeError(f"{relative} does not import the catalog exactly once")
        module.write_text(
            source.replace(needle, needle.replace("i18n.js", f"i18n.{language}.js")),
            encoding="utf-8",
        )


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
    bun = shutil.which("bun")
    if bun is None:
        raise RuntimeError("bun is not on PATH; needed to run the generator")
    generator = BUILD_DIR / "ssg" / "src" / "main.js"
    if not generator.is_file():
        raise FileNotFoundError(f"{generator} is missing; run the TypeScript build")
    stamp, stamp_url = build_stamp(project_root_dir)
    command = [
        bun,
        str(generator),
        "--docs",
        str(GUIDE_DIR),
        "--dist",
        str(dist_dir),
        "--site",
        str(SITE_DIR),
        "--repo-root",
        str(project_root_dir),
        "--alcy",
        str(project_root_dir / "src" / "playground" / "js" / "alcy.mjs"),
        "--stamp",
        stamp,
        "--stamp-url",
        stamp_url,
    ]
    grammar = dist_dir / PAGE_SUBDIR / "grammar"
    if (grammar / "tree-sitter-alcy.wasm").is_file():
        command += [
            "--grammar",
            str(grammar / "tree-sitter-alcy.wasm"),
            "--highlights",
            str(grammar / "highlights.scm"),
            "--tree-sitter",
            str(DEPS_DIR / "node_modules" / "web-tree-sitter" / "web-tree-sitter.js"),
        ]
    else:
        log("guide: grammar wasm is missing; code fences are not highlighted")
    glue = dist_dir / PAGE_SUBDIR / "compiler" / "alcy_playground.js"
    if glue.is_file():
        command += ["--glue", str(glue)]
    else:
        log("guide: compiler wasm is missing; alcy fences were not compiled")
    subprocess.run(command, check=True, cwd=project_root_dir)
    # English is the tree at the site root, so its modules are the ones
    # the build already copied; every other language lives under its own
    # prefix, which is how `tools/site.py` names the trees it stamps.
    for language in SITE_LANGUAGES:
        route = "" if language == DEFAULT_LANGUAGE else f"{language}/"
        stamp_catalog(dist_dir, route, language)


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
    build_typescript(modules)

    dist_dir = args.dist.resolve()
    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    dist_dir.mkdir(parents=True)

    copy_static(dist_dir)
    copy_shared(dist_dir)
    write_catalogs(dist_dir)
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

    errors: list[str] = []
    for budget in PAGE_BUDGETS + ASSET_BUDGETS:
        check_budget(dist_dir, budget, errors)
    check_guide_scripts(dist_dir, errors)
    if errors:
        for message in errors:
            print(f"site: {message}", file=sys.stderr)
        return 1

    log(f"site ready: {dist_dir}")
    log(f"serve it with: uv run ./tools/site.py serve --dist {dist_dir}")
    return 0


def command_test(_args: argparse.Namespace) -> int:
    versions = load_versions()
    modules = install_dependencies(versions)
    build_typescript(modules)
    bun = shutil.which("bun")
    if bun is None:
        raise RuntimeError("bun is not on PATH; needed to run the tests")
    subprocess.run(
        [bun, "test", str(BUILD_DIR / "ssg" / "test")],
        check=True,
        cwd=project_root_dir,
    )
    log("generator tests passed")
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
