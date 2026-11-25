# winwm

Tiling window manager and shell for Windows x64.

| Task | PowerShell |
| --- | --- |
| Build (MinGW-w64 GCC and windres) | `.\build.bat` |
| Install for next sign-in | `.\bin\winwm_install.exe /install` |
| Run as shell | `.\bin\winwm.exe` |
| Preview alongside Explorer | `.\bin\winwm.exe --preview` |

## Features

- Per-monitor workspaces, tiling, floating, mouse move and resize.
- Hover to focus and center the pointer. Desktop gaps clear focus.
- Configurable borders, animations, keybindings, and toolbar.
- Win+D Menu for apps, commands, WinGet installs and updates.
- Audio, network, displays, power, languages, keyboards, and IMEs.
- Clipboard history, screen capture, recording, and process monitoring.

## Config

`%USERPROFILE%\.config\winwm\winwm.conf` · Reload with Win+Shift+R.

```ini
general:gaps_in = 6
bar:modules_right = network audio keyboard battery
```
