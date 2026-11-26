"""Enforce dependency direction for production headers and compiled C modules."""

from collections import defaultdict
from graphlib import CycleError, TopologicalSorter
from pathlib import Path
import re
import subprocess
import sys


include = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(?:"([^"\n]+)"|<([^>\n]+)>)', re.MULTILINE)
comments_and_literals = re.compile(r'''"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|//[^\n]*|/\*[\s\S]*?\*/''')


def remove_comment(match):
    value = match[0]
    return " " + "\n" * value.count("\n") if value.startswith("/") else value


# From foundations to entry points. Modules on the same row are peers.
levels = (
    ("base",),
    ("model", "command", "os_win32", "tray", "ipc"),
    ("layout", "config", "native_notifications"),
    ("input", "lua_config"),
    ("win_platform",),
    ("providers", "catalog", "theme"),
    ("bar", "palette", "notifications", "install"),
    ("desktop_tools", "desktop_ctl", "shell_host"),
    ("desktop",),
    ("app",),
)
rank = {name: i for i, names in enumerate(levels) for name in names}
portable = {"base", "model", "command", "config", "layout"}


def dependencies(source, root):
    text = source.read_text(encoding="utf-8-sig").replace("\\\n", "")
    for quoted, angled in include.findall(comments_and_literals.sub(remove_comment, text)):
        spelling = quoted or angled
        for directory in ((source.parent, root) if quoted else (root,)):
            target = (directory / spelling).resolve()
            if target.is_file():
                if target.is_relative_to(root):
                    yield target
                break
        else:
            if spelling.partition("/")[0] in rank:
                raise SystemExit(f"{source}: unresolved project header {spelling}")


def module(path):
    return path.parts[0] if len(path.parts) > 1 else "."


def linked_modules(files, object_root):
    # Use production translation units, so stale and third-party objects cannot
    # create phantom edges. A missing object makes the check fail.
    objects = [path.with_suffix(".o") for path in files if path.suffix == ".c"]
    output = subprocess.check_output(
        ["nm", "--extern-only", "--print-file-name", "--format=posix", *map(str, objects)],
        cwd=object_root, text=True,
    )
    definitions = defaultdict(set)
    references = defaultdict(set)
    for line in output.splitlines():
        filename, record = line.split(": ", 1)
        symbol, kind, *_ = record.split()
        owner = module(Path(filename))
        if kind == "U":
            references[owner].add(symbol)
        elif symbol.isidentifier():
            definitions[symbol].add(owner)
    for owner, symbols in references.items():
        for symbol in sorted(symbols):
            for provider in definitions.get(symbol, ()):
                if provider != owner:
                    yield owner, provider, symbol


def check(root, object_root=None):
    root = root.resolve()
    pending = sorted(path for path in root.rglob("*") if path.suffix in {".c", ".h"} and path.is_file())
    if not pending:
        raise SystemExit(f"No C sources or headers in {root}")
    files = {}
    modules = defaultdict(dict)
    while pending:
        source = pending.pop()
        name = source.relative_to(root)
        if name in files:
            continue
        if module(name) not in rank:
            raise SystemExit(f"{name}: assign the module a dependency level")
        files[name] = set()
        for target in dependencies(source, root):
            dependency = target.relative_to(root)
            files[name].add(dependency)
            if module(name) != module(dependency):
                modules[module(name)][module(dependency)] = str(name)
            if dependency not in files:
                pending.append(target)
    if object_root:
        for owner, provider, symbol in linked_modules(files, object_root):
            modules[owner][provider] = symbol
    for label, graph in (("File", files), ("Module", modules)):
        try:
            TopologicalSorter(graph).prepare()
        except CycleError as error:
            chain = " -> ".join(map(str, error.args[1]))
            raise SystemExit(f"{label} dependency cycle: {chain}") from None
    for owner, edges in modules.items():
        for provider, origin in edges.items():
            if rank[provider] >= rank[owner] or (owner in portable and provider not in portable):
                raise SystemExit(f"Invalid dependency {owner} -> {provider} ({origin})")
    print(f"Checked {len(files)} source files, {len({module(p) for p in files})} modules"
          + (" and compiled symbol dependencies." if object_root else "."))


if __name__ == "__main__":
    check(Path(sys.argv[1]), Path(sys.argv[2]) if len(sys.argv) > 2 else None)
