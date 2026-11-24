"""Check module boundaries and the translation units in each Windows executable."""
import json
from pathlib import Path
import re
import sys

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


def check_graph(module, path, visited):
    if module in path:
        sys.exit(f"Dependency cycle: {' -> '.join([*path, module])}")
    if module not in visited:
        for dependency in dependencies[module]:
            check_graph(dependency, [*path, module], visited)
        visited.add(module)


visited = set()
for module in dependencies:
    check_graph(module, [], visited)

for file in sorted(source.rglob("*")):
    if file.suffix not in {".h", ".c"}:
        continue
    module = file.relative_to(source).parts[0]
    if module not in dependencies:
        sys.exit(f"{file}: unregistered module {module}")
    text = file.read_text()
    allowed = {*dependencies[module], module}
    for name in re.findall(r'^\s*#include\s*"([^"]+)"', text, re.M):
        if name.split("/")[0] not in allowed or not (source / name).is_file():
            sys.exit(f"{file}: illegal include {name}")
    if module in portable and re.search(r'windows\.h|\b(HWND|HANDLE|WPARAM|RECT|HDC|LPARAM)\b', text):
        sys.exit(f"{file}: platform type in portable layer")

units = {file.relative_to(project).as_posix() for file in source.rglob("*.c")}
install_units = {name for name in units if name.startswith("src/install/")}
for response, expected in [("build.rsp", units - install_units), ("install.rsp", install_units)]:
    listed = [line for line in (project / response).read_text().splitlines() if line.endswith(".c")]
    if sorted(listed) != sorted(expected):
        sys.exit(f"{response}: each translation unit for this executable must be listed exactly once")

template = (project / "assets/winwm.conf").read_text()
embedded = "".join(json.loads(line) for line in (source / "config/default_conf.inc").read_text().splitlines())
if embedded != template:
    sys.exit("Embedded first-run configuration differs from assets/winwm.conf")
print("Module dependencies, compilation units and embedded configuration verified")
