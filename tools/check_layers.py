"""Check module boundaries and the translation units in each Windows executable."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from graphlib import TopologicalSorter
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory

project = Path(__file__).resolve().parents[1]
source = project / "src"
dependencies = {
    "base": [],
    "model": ["base"],
    "command": ["base"],
    "config": ["base", "command"],
    "layout": ["model"],
    "os_win32": ["base"],
    "theme": ["os_win32"],
    "win_platform": ["config", "os_win32"],
    "ipc": ["win_platform"],
    "catalog": ["win_platform"],
    "input": ["win_platform"],
    "providers": ["win_platform", "base"],
    "bar": ["win_platform"],
    "palette": ["win_platform", "theme"],
    "desktop_ctl": ["ipc"],
    "desktop_tools": ["win_platform", "theme"],
    "desktop": ["os_win32", "theme", "layout", "bar", "palette", "ipc", "providers", "input", "catalog"],
    "shell_host": ["os_win32", "win_platform", "theme"],
    "app": ["desktop", "desktop_ctl", "desktop_tools", "shell_host"],
    "install": [],
}
portable = {"base", "model", "command", "config", "layout"}


def check_graph(graph):
    visited = set()
    path = []

    def visit(node):
        if node in path:
            cycle = path[path.index(node):] + [node]
            raise ValueError(f"Dependency cycle: {' -> '.join(map(str, cycle))}")
        if node in visited:
            return
        if node not in graph:
            raise ValueError(f"Unknown dependency: {node}")
        path.append(node)
        for target in sorted(graph[node]):
            visit(target)
        path.pop()
        visited.add(node)

    for node in sorted(graph):
        visit(node)


# C splices lines before replacing comments. Preserve strings and character
# literals so a comment delimiter inside a string cannot hide a real include.
c_tokens = re.compile(r'''("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|//[^\n]*|/\*[\s\S]*?\*/''')
directives = re.compile(r'^[ \t]*#[ \t]*(include|import|include_next)\b([^\n]*)', re.M)


def include_target(source_root, file, argument):
    header = re.fullmatch(r'"([^"]+)"|<([^>]+)>', argument.strip())
    if not header:
        raise ValueError(f"{file}: cannot resolve computed include {argument.strip()}")
    quoted, system = header.groups()
    name = Path((quoted or system).replace("\\", "/"))
    roots = (file.parent, source_root) if quoted else (source_root,)
    for root in roots:
        candidate = (root / name).resolve()
        if candidate.is_file():
            if not candidate.is_relative_to(source_root):
                raise ValueError(f"{file}: include leaves source tree: {name}")
            return candidate
    if quoted or not name.parts or name.is_absolute() or ".." in name.parts or name.parts[0] in dependencies:
        raise ValueError(f"{file}: unresolved project include {name}")
    return None


def check_sources(source_root):
    source_root = source_root.resolve()
    files = {}
    modules = {name: set() for name in dependencies}
    forbidden = []
    pending = sorted(f for f in source_root.rglob("*") if f.suffix in {".h", ".c", ".inc"})
    while pending:
        file = pending.pop()
        relative = file.relative_to(source_root)
        if relative in files:
            continue
        module = relative.parts[0]
        if module not in dependencies:
            raise ValueError(f"{relative}: unregistered module {module}")
        text = file.read_text(encoding="utf-8").replace("\\\n", "")
        text = c_tokens.sub(lambda m: m[1] or " " + "\n" * m[0].count("\n"), text)
        files[relative] = set()
        for directive, argument in directives.findall(text):
            if directive == "include_next":
                raise ValueError(f"{relative}: cannot resolve #include_next against project headers")
            target = include_target(source_root, file, argument)
            if target is None:
                if module in portable and argument.strip().lower() == "<windows.h>":
                    raise ValueError(f"{relative}: Windows header in portable layer")
                continue
            pending.append(target)
            target = target.relative_to(source_root)
            files[relative].add(target)
            dest = target.parts[0]
            if dest != module:
                modules[module].add(dest)
                if dest not in dependencies[module]:
                    forbidden.append(f"{relative}: illegal include {target} ({module} -> {dest})")
        if module in portable and re.search(r'\b(HWND|HANDLE|WPARAM|RECT|HDC|LPARAM)\b', c_tokens.sub(" ", text)):
            raise ValueError(f"{relative}: platform type in portable layer")
    check_graph(files)
    check_graph(modules)
    if forbidden:
        raise ValueError("\n".join(forbidden))
    return modules


def compile_symbols(source, directory, compiler, nm):
    obj = directory / source.with_suffix(".o")
    obj.parent.mkdir(exist_ok=True, parents=True)
    subprocess.run([compiler, "@compile.rsp", "-g",
                    "-c", str(source), "-o", str(obj)], cwd=project, check=True)
    listing = subprocess.check_output([nm, "-g", "-P", "-l", str(obj)], text=True)
    defined, required = set(), set()
    for line in listing.splitlines():
        fields, _, location = line.partition("\t")
        symbol, kind, *_ = fields.split()
        if symbol.startswith("."):
            continue
        if kind == "U":
            required.add(symbol)
        elif kind in {"T", "D", "B", "R", "C", "S", "G"}:
            # MinGW instantiates SDK GUIDs in objects that include <initguid.h>.
            # Their declaration locations distinguish them from project definitions.
            if location:
                origin = Path(location.strip().rsplit(":", 1)[0])
                if not (project / origin).resolve().is_relative_to(project / "src"):
                    continue
            defined.add(symbol)
    return source, defined, required


def check_target(objects, allowed):
    owners = {symbol: source for source, defined, _ in objects for symbol in defined}
    graph = {source.parts[1]: set() for source, _, _ in objects}
    for source, _, required in objects:
        origin = source.parts[1]
        for symbol in sorted(required & owners.keys()):
            destination = owners[symbol].parts[1]
            if origin == destination:
                continue
            if destination not in allowed[origin]:
                raise ValueError(f"{source} references {symbol} in forbidden module {destination}")
            graph[origin].add(destination)
    check_graph(graph)
    return sum(map(len, graph.values()))


def check_linked():
    compiler = os.environ.get("CC", "gcc")
    nm = os.environ.get("NM") or subprocess.check_output(
        [compiler, "-print-prog-name=nm"], text=True).strip()
    allowed = {module: set(parents) for module, parents in dependencies.items()}
    for module in TopologicalSorter(dependencies).static_order():
        for parent in dependencies[module]:
            allowed[module].update(allowed[parent])
    with TemporaryDirectory() as directory, ThreadPoolExecutor(max_workers=4) as pool:
        for response in ("build.rsp", "install.rsp"):
            units = [Path(line) for line in (project / response).read_text().splitlines() if line.endswith(".c")]
            jobs = [pool.submit(compile_symbols, unit, Path(directory), compiler, nm) for unit in units]
            edges = check_target([job.result() for job in jobs], allowed)
            print(f"{response}: {len(units)} compiled units, {edges} cross-module linkage edges, acyclic")


def main():
    check_graph(dependencies)
    check_sources(source)
    units = {file.relative_to(project).as_posix() for file in source.rglob("*.c")}
    install_units = {name for name in units if name.startswith("src/install/")}
    for response, expected in [("build.rsp", units - install_units), ("install.rsp", install_units)]:
        listed = [line.strip() for line in (project / response).read_text().splitlines() if line.strip().endswith(".c")]
        if sorted(listed) != sorted(expected):
            raise ValueError(f"{response}: each translation unit for this executable must be listed exactly once")

    template = (project / "assets/winwm.conf").read_text(encoding="utf-8")
    embedded = "".join(json.loads(line) for line in (source / "config/default_conf.inc").read_text(encoding="utf-8").splitlines())
    if embedded != template:
        raise ValueError("Embedded first-run configuration differs from assets/winwm.conf")
    print("Module and header DAGs, compilation units and embedded configuration verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linked", action="store_true", help="also verify compiled MinGW-w64 symbol references")
    args = parser.parse_args()
    try:
        main()
        if args.linked:
            check_linked()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
