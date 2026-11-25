"""Offline, non-executing configuration converter. Writes native JSON to stdout."""
import argparse
import json
import re
import sys
from pathlib import Path

comment = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', re.S)
trailing = re.compile(r'"(?:\\.|[^"\\])*"|,\s*(?=[}\]])', re.S)

def read_jsonc(source):
    source = comment.sub(lambda m: m[0] if m[0][0] == '"' else "\n" * m[0].count("\n"), source)
    return json.loads(trailing.sub(lambda m: m[0] if m[0][0] == '"' else "", source))

class Importer:
    def __init__(self):
        self.config = {"version": 1}
        self.issues = []
        self.variables = {}

    def report(self, path, line, message):
        self.issues.append(f"{path}:{line}: {message}")

    def load(self, path, kind, stack=()):
        path = path.resolve()
        if len(stack) >= 16 or path in stack:
            self.report(path, 1, "include cycle or depth limit")
            return
        if path.stat().st_size > 256 * 1024:
            raise ValueError(f"{path}: input exceeds 256 KiB")
        source = path.read_text(encoding="utf-8-sig")
        if kind == "bar":
            self.bar(path, source)
        elif kind == "style":
            self.style(path, source)
        else:
            if kind == "lua" and any(re.search(r"^\s*(?:local\s+function|function|if|for|while|repeat|do)\b", raw) for raw in source.splitlines()):
                self.report(path,1,"dynamic Lua control flow requires manual conversion")
                return
            for n, raw in enumerate(source.splitlines(), 1):
                line = raw.strip()
                if not line or line.startswith(("#", "--")) or line == "}" or line.endswith("{"):
                    continue
                if line.startswith("bindsym "):
                    fields = line.split(None, 2)
                    if len(fields) == 3:
                        chord = fields[1].lower().replace("$mod+", "super+").replace("+return", "+enter")
                        actions = {"exec $term": "terminal", "kill": "close", "exec $menu": "launcher",
                                   "focus right": "focus right", "lock": "lock"}
                        action = actions.get(fields[2])
                        executable = re.fullmatch(r"exec\s+([\w.-]+)", fields[2])
                        if executable:
                            action = "launch " + executable[1]
                        if action and re.fullmatch(r"(?:super|ctrl|alt|shift)(?:[+](?:super|ctrl|alt|shift))*[+](?:[a-z0-9]|enter|left|right|up|down)", chord):
                            self.config.setdefault("bindings", {})[chord] = action
                            continue
                    self.report(path, n, "unmapped bindsym")
                    continue
                variable = re.fullmatch(r"(\$\w+)\s*=\s*(.+)", line)
                if variable:
                    self.variables[variable[1]] = variable[2]
                    continue
                line = re.sub(r"\$\w+", lambda m: self.variables.get(m[0], m[0]), line)
                key, sep, val = line.partition("=")
                key, val = key.strip(), val.strip()
                if sep and key == "source" and not any(x in val for x in "$*?"):
                    try:
                        self.load(path.parent / val, kind, (*stack, path))
                    except OSError as e:
                        self.report(path, n, str(e))
                elif key in {"gaps_in", "gaps_out"} and val.isdigit() and int(val) <= 128:
                    self.config["gap"] = int(val)
                    self.report(path, n, "mapped to shared gap setting")
                elif key in {"windowrule","windowrulev2"}:
                    fields=[x.strip() for x in val.split(",")]
                    if len(fields)!=2:
                        self.report(path,n,"rule needs an explicit native mapping")
                        continue
                    action, match=fields
                    name, _, pattern=match.partition(":")
                    if name not in {"title","class"} or not pattern or re.search(r"[.\[\]{}^$*+?|\\()]",pattern):
                        self.report(path,n,"regular-expression rule needs a native match")
                        continue
                    rule={name:pattern}
                    if action=="float": rule["floating"]=True
                    elif re.fullmatch(r"workspace (?:[1-9]|10)",action): rule["workspace"]=int(action.split()[-1])
                    else:
                        self.report(path,n,"unmapped window rule")
                        continue
                    self.config.setdefault("rules",[]).append(rule)
                elif key in {"bind", "binde"}:
                    terms = [v.strip() for v in val.split(",", 3)]
                    self.binding(path, n, terms, repeat=key == "binde")
                elif kind == "lua":
                    call = re.fullmatch(r"""(?:\w+\.)?bind\(\s*["']([^"']+)["']\s*,\s*(?:\w+\.)?(?:dsp\.)?(\w+)\(\s*(?:["']?([\w-]+)["']?)?\s*\)\s*\)\s*""", line)
                    if call:
                        *mods, key_name = [part.strip() for part in call[1].split("+")]
                        self.binding(path, n, [" ".join(mods), key_name, call[2], call[3] or ""])
                    else:
                        self.report(path, n, "unmapped Lua expression")
                else:
                    self.report(path, n, f"unmapped directive {key}")

    def binding(self, path, line, parts, repeat=False):
        if len(parts) < 3:
            self.report(path, line, "incomplete binding")
            return
        mods, key, dispatch = parts[:3]
        arg = parts[3] if len(parts) > 3 else ""
        actions = {"killactive": "close", "togglefloating": "floating",
                   "fullscreen": "fullscreen", "togglespecialworkspace": "scratch_show"}
        action = actions.get(dispatch)
        if dispatch in {"workspace", "movetoworkspace", "movetoworkspacesilent"} and arg.isdigit() and 1 <= int(arg) <= 10:
            action = ("workspace_select " if dispatch == "workspace" else "workspace_send ") + arg
        directions = {"l": "left", "r": "right", "u": "up", "d": "down"}
        if dispatch in {"movefocus", "movewindow"} and arg in directions:
            action = ("focus " if dispatch == "movefocus" else "move ") + directions[arg]
        if dispatch == "exec":
            action = {"terminal": "terminal", "file_manager": "open_folder", "launcher": "launcher"}.get(arg)
            if action is None and re.fullmatch(r"[\w.-]+",arg):
                action = "launch " + arg
        if not action:
            self.report(path, line, f"map {dispatch} {arg} to a Windows action")
            return
        key = {"return": "enter"}.get(key.lower(), key.lower())
        mod_list = [m.lower() for m in mods.split()]
        chord = "+".join([*mod_list, key])
        keys = {"enter","escape","space","left","up","right","down","minus","plus","tab","print","delete","backspace"}
        if (set(mod_list) - {"super","shift","ctrl","alt"} or (not mod_list and key != "print") or
                (key not in keys and not re.fullmatch(r"[a-z0-9]|f(?:[1-9]|1\d|2[0-4])",key))):
            self.report(path,line,f"unavailable binding {chord}")
            return
        bindings = self.config.setdefault("bindings", {})
        bindings.pop(chord if repeat else "repeat:" + chord, None)
        bindings[("repeat:" if repeat else "") + chord] = action

    def bar(self, path, source):
        root = read_jsonc(source)
        if not isinstance(root, dict):
            raise ValueError(f"{path}: expected one bar object")
        known = {"workspaces","mode","clock","battery","memory","keyboard","title","cpu","audio","network","media","throughput"}
        aliases={"window":"title","language":"keyboard","pulseaudio":"audio","wireplumber":"audio","mpris":"media"}
        def module_name(name):
            short=str(name).split("/")[-1]
            base, mark, instance=short.partition("#")
            return aliases.get(base,base)+mark+instance
        for key, value in root.items():
            line = source[:source.find(json.dumps(key))].count("\n") + 1
            if key in {"modules-left","modules-center","modules-right"} and isinstance(value, list):
                mapped = []
                for module in value:
                    short=module_name(module)
                    if short.split("#")[0] in known and not str(module).startswith("custom/"):
                        mapped.append(short)
                    else:
                        self.report(path,line,f"unmapped module {module}")
                self.config[key.replace("modules-","bar_")] = " ".join(mapped)
            elif isinstance(value,dict) and module_name(key).split("#")[0] in known:
                converted={}
                for option, setting in value.items():
                    if option=="format" and isinstance(setting,str):
                        rendered=setting.replace("{}", "{text}")
                        if rendered == "{:%H:%M}": rendered="{text}"
                        if re.search(r"[{}]",rendered.replace("{text}","")):
                            self.report(path,line,f"unmapped format field in {key}")
                        else: converted["format"]=rendered
                    elif option=="tooltip-format" and isinstance(setting,str) and "{" not in setting:
                        converted["tooltip"]=setting
                    else: self.report(path,line,f"unmapped option {key}.{option}")
                if converted: self.config.setdefault("modules",{}).setdefault(module_name(key),{}).update(converted)
            elif key == "position" and value in {"left","top","right","bottom"}:
                self.config["edge"] = value
            elif key == "height" and type(value) is int and 16 <= value <= 128:
                self.config["bar_height"] = value
            else:
                self.report(path,line,f"unmapped bar option {key}")

    def style(self, path, source):
        source = re.sub(r"/\*.*?\*/",lambda m:"\n"*m[0].count("\n"),source,flags=re.S)
        last = 0
        for block in re.finditer(r"([^{}]+)\{([^{}]*)\}",source):
            selector = block[1].strip()
            line = source[:block.start(2)].count("\n") + 1
            for statement in block[2].split(";"):
                if not statement.strip():
                    continue
                key, _, value = statement.strip().partition(":")
                value = value.strip()
                token = None
                if selector in {"*", "window"} or re.fullmatch(r"window#[\w-]+",selector):
                    token = {"background":"background","background-color":"background","color":"foreground"}.get(key)
                elif selector in {"#workspaces button.active","#workspaces button.focused"} and key in {"background","background-color"}:
                    token = "accent"
                if re.fullmatch(r"#[a-z_][a-z_0-9-]*",selector) and key=="color" and re.fullmatch(r"#[0-9a-fA-F]{6}",value):
                    self.config.setdefault("modules",{}).setdefault(selector[1:],{})["color"]=value
                    continue
                if token and re.fullmatch(r"#[0-9a-fA-F]{6}",value):
                    self.config[token] = value
                else:
                    self.report(path,line,f"unmapped style {selector} / {key}")
            if source[last:block.start()].strip():
                self.report(path,line,"unmapped CSS text")
            last = block.end()
        if source[last:].strip():
            self.report(path,source[:last].count("\n")+1,"unmapped CSS text")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for kind in ("wm","lua","bar","style"):
        parser.add_argument("--"+kind, type=Path, action="append", default=[])
    args = parser.parse_args()
    converter = Importer()
    try:
        for kind, paths in vars(args).items():
            for path in paths:
                converter.load(path,kind)
    except (OSError,ValueError,TypeError) as error:
        parser.exit(2,f"{error}\n")
    for issue in converter.issues:
        print(issue,file=sys.stderr)
    print(json.dumps(converter.config,ensure_ascii=False,indent=2))
    return 1 if converter.issues else 0

if __name__ == "__main__":
    sys.exit(main())

