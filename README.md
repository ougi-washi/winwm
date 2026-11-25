# winwm

Tiling window manager and desktop shell for Windows x64.

## Build

Requires MinGW-w64 `gcc` and `windres` on `PATH`.

```powershell
.\build.bat
```

## Install

```powershell
.\bin\winwm_install.exe /install
```

Sign out and back in to start winwm as your shell.

## Run

```powershell
.\bin\winwm.exe            # Replace Explorer for this session
.\bin\winwm.exe --preview  # Run alongside Explorer
```

## Features

- Per-monitor workspaces, tiling, floating, fullscreen, scratchpad.
- Hover focus. Win+left-drag to move. Win+right-drag to resize.
- Configurable borders, gaps, animations, and keybindings.
- Run menu (Win+D), app search, command execution, project launcher.
- Toolbar controls for audio, input languages, Wi-Fi, Bluetooth, displays, and power.
- Activity monitor, clipboard history, screenshots, screen recording, color picker.

## Config

`%USERPROFILE%\.config\winwm\winwm.conf` · Reload with Win+Shift+R.

```ini
general {
    gaps_in = 6
    border_size = 2
    col.active_border = rgb(91aaf7)
}
input {
    follow_mouse = 1
}
```
