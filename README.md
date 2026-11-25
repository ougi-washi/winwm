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
- Menu (Win+D). Search apps and actions. Type `> executable` to run a command.
- Software. Search WinGet, install packages, review updates.
- Languages & input. Add or remove languages. Select keyboards and IMEs. Install basic typing. Test input.
- Configurable toolbar. Hover for details. Middle-click Menu to edit module order.
- Icon controls show their action on hover or keyboard focus.
- Custom toolbar commands appear in Menu under Toolbar actions.
- Output, microphone, and app volume controls. Input languages, Wi-Fi, Bluetooth, displays, and power.
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
bar {
    modules_right = cpu network audio keyboard battery
    audio:display = icon_text
    audio:on_click_middle = mute
    tooltip_duration = 4500
}
```
