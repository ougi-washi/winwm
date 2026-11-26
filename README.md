# winwm

Windows x64 · Tiling · Per-monitor workspaces · Tabs · Floating windows · App menu · Status bar

## Build

`gcc` · `windres` · `mingw32-make` (MinGW-w64)

```powershell
.\build.bat
```

## Install

```powershell
.\bin\winwm.exe install
.\bin\winwm.exe uninstall
.\bin\winwm.exe rollback
```

**Win+D** → `install winwm` · `uninstall winwm` · `update winwm`

## Run

```powershell
.\bin\winwm.exe
.\bin\winwm.exe --preview
.\bin\winwm.exe menu
.\bin\winwm.exe reload
.\bin\winwm.exe quit
.\bin\winwm.exe --help
```

Normal startup gives winwm the full monitor and reserves space for its configured bar. Once its input bindings and bars are ready, winwm closes the Windows Explorer shell and watches for it to return. Separately opened File Explorer folder windows can stay open. Exiting winwm restores the Windows desktop. Use `--preview` to run alongside Explorer and `winwm.exe doctor` to check the current shell and tray.

**Windows Settings** opens winwm’s device and system controls. **Windows Update** shows recent history and checks for available updates. **Windows Security** shows protection status and firewall profiles, opens the firewall console, and can start a Microsoft Defender quick scan or update its security intelligence. These panels run without Explorer in a normal winwm session.

## Architecture

The desktop thread owns workspace selection, layout, and the focused window. The input thread matches shortcuts and queues commands. Native window events report visibility and activation back to the desktop thread. The portable model and layout code can run without a Windows SDK or a desktop session. Window placement, input delivery, and composition use Windows APIs.

The input thread receives keyboard and pointer events through low-level hooks. Registering a raw keyboard input sink in the same process can suppress those hook callbacks when a winwm window takes focus, including on an empty workspace.

Production modules have explicit dependency levels in `tools/check_includes.py`. CI checks project includes, compiles the portable core on Linux, and checks references in the Windows object files. To run the full dependency check locally after a build, use `python tools/check_includes.py src build/obj` with MinGW’s `nm` on the path.

## Config

`~/.config/winwm/winwm.lua`

```lua
local mod = "SUPER + "
local dsp = hl.dsp
hl.config({
    general = { layout = "dwindle", gaps_in = 2, gaps_out = 2, border_size = 1 },
    decoration = { rounding = 2, inactive_opacity = 0.95,
        panels = { button_size = 32, spacing = 6, opacity = 0.94, blur = true, shadow = true } },
    notifications = { capture = "windows", corner = "top_right", duration_ms = 5000, fade_ms = 160, history_limit = 128 },
    bar = {
        modules_left = { "menu", "workspaces" },
        modules_center = { "clock" },
        modules_right = { "tray", "cpu", "network", "audio", "keyboard", "battery", "notifications" },
    },
})
hl.bind(mod .. "D", dsp.winwm("menu"))
hl.bind(mod .. "Return", dsp.winwm("terminal"))
hl.bind(mod .. "SHIFT + R", dsp.reload_config())
hl.bind(mod .. "SHIFT + Space", dsp.window.float())
hl.bind(mod .. "mouse:272", dsp.window.drag(), { mouse = true })
hl.bind(mod .. "mouse:273", dsp.window.resize(), { mouse = true })
for i = 1, 10 do
    hl.bind(mod .. (i % 10), dsp.focus({ workspace = i }))
end
```
