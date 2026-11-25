# winwm

Windows x64 · Tiling · Per-monitor workspaces · Tabs · Floating windows · App menu · Status bar

## Build

`gcc` · `windres` · `mingw32-make` (MinGW-w64)

```powershell
.\build.bat
```

## Install

```powershell
.\bin\winwm_install.exe /install
.\bin\winwm_install.exe /uninstall
```

## Run

```powershell
.\bin\winwm.exe
.\bin\winwm.exe --preview
.\bin\winwm.exe menu
.\bin\winwm.exe reload
.\bin\winwm.exe quit
.\bin\winwm.exe --help
```

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
