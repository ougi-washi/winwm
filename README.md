# winwm

Tiling window manager for Windows x64. Normal startup exits Explorer. Exiting winwm restores Explorer.

| Task | PowerShell |
| --- | --- |
| Parallel build (MinGW-w64 GCC, windres, mingw32-make) | `.\build.bat` · Limit jobs with `.\build.bat -j4` |
| Start with Windows at sign-in | `.\bin\winwm_install.exe /install` |
| Uninstall | `.\bin\winwm_install.exe /uninstall` |
| Run | `.\bin\winwm.exe` |
| Exit to Windows | Menu → Session → Exit winwm, or `.\bin\winwm.exe ctl run quit` |
| Preview alongside Explorer | `.\bin\winwm.exe --preview` |

## Features

- Per-monitor workspaces, tiling, floating, mouse move and resize.
- Win+W shows tiled window titles as tabs. Click a tab to switch windows.
- Hover to focus. Desktop gaps preserve focus.
- Configurable borders, animations, keybindings, and toolbar.
- Win+D Menu for apps, commands, WinGet installs and updates, and elevated Chocolatey installs.
- Menu → Settings includes Windows Security, Windows Settings, and Control Panel. Use `--preview` for panels that require Explorer.
- Ctrl+Enter in Menu runs the selected app, tool, or command as administrator.
- Win+Shift+Q closes the active panel or app.
- Audio, network, displays, power, languages, keyboards, and IMEs.
- Clipboard history, screen capture, recording, and process monitoring.

## Config

`%USERPROFILE%\.config\winwm\winwm.conf` · Reload with Win+Shift+R.

Updates reuse the existing config and its sourced files.

```ini
general:gaps_in = 6
bar:modules_right = network audio keyboard battery
```
