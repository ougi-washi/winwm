"""Check the file and module DAGs formed by project-local quoted C includes."""

from collections import defaultdict
from graphlib import CycleError, TopologicalSorter
from pathlib import Path
import re
import sys


include = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"\n]+)"', re.MULTILINE)
comments_and_literals = re.compile(r'''"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|//[^\n]*|/\*[\s\S]*?\*/''')


def remove_comment(match):
    value = match[0]
    return " " + "\n" * value.count("\n") if value.startswith("/") else value


def dependencies(source, root):
    text = source.read_text(encoding="utf-8-sig").replace("\\\n", "")
    for spelling in include.findall(comments_and_literals.sub(remove_comment, text)):
        for directory in (source.parent, root):
            target = (directory / spelling).resolve()
            if target.is_file():
                if target.is_relative_to(root):
                    yield target
                break


def module(path):
    return path.parts[0] if len(path.parts) > 1 else "."


def check(root):
    root = root.resolve()
    pending = sorted(path for path in root.rglob("*") if path.suffix in {".c", ".h"} and path.is_file())
    if not pending:
        raise SystemExit(f"No C sources or headers in {root}")
    files = {}
    modules = defaultdict(set)
    while pending:
        source = pending.pop()
        name = source.relative_to(root)
        if name in files:
            continue
        files[name] = set()
        for target in dependencies(source, root):
            dependency = target.relative_to(root)
            files[name].add(dependency)
            if module(name) != module(dependency):
                modules[module(name)].add(module(dependency))
            if dependency not in files:
                pending.append(target)
    for label, graph in (("File", files), ("Module", modules)):
        try:
            TopologicalSorter(graph).prepare()
        except CycleError as error:
            chain = " -> ".join(map(str, error.args[1]))
            raise SystemExit(f"{label} include cycle: {chain}") from None


if __name__ == "__main__":
    check(Path(sys.argv[1]))
