# winwm

Tiling window manager for Windows x64. Per-monitor workspaces, tabbed and floating windows, app menu, configurable bar.

## Build

MinGW-w64 GCC, windres, mingw32-make.

```powershell
.\build.bat
```

## Run

- Start winwm and exit Explorer — `.\bin\winwm.exe`
- Preview alongside Explorer — `.\bin\winwm.exe --preview`
- Quit and restore Explorer — `.\bin\winwm.exe ctl run quit`
- Install at sign-in — `.\bin\winwm_install.exe /install`
- Uninstall — `.\bin\winwm_install.exe /uninstall`

## Config

Edit `~/.config/winwm/winwm.lua`. Reload with `Win+Shift+R`. [Hyprland Lua API subset](https://wiki.hypr.land/configuring/)

`programs.terminal = "auto"` uses the Windows default terminal host. Set an executable path to override it.

The toolbar editor saves module placement in `toolbar-layout.lua`. It loads last.

Hover the tray arrow for minimized windows and background apps. Scroll for more icons. Right-click for app actions. Preview keeps native tray icons in Explorer.

Open the bell or **Menu → Notifications** for searchable session history. Set `duration_ms = 0` for sticky toasts or `fade_ms = 0` for instant transitions.

**Capture options → Read Windows notifications** requests Windows access. Set `capture = "visible"` to log on-screen Windows banners or `"off"` for winwm notices only.

Panel opacity ranges from 0 to 1. Acrylic requires Windows 11 22H2+ and Transparency effects. Framed tools use the Windows frame shadow.

```lua
local mod = "SUPER + "
local dsp = hl.dsp
hl.config({
    general = { layout = "dwindle", gaps_in = 6, border_size = 2 },
    decoration = { rounding = 8, panels = { opacity = 0.94, blur = true, shadow = true } },
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
