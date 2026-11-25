# winwm

A tiling window manager and desktop shell for Windows x64.

- Build with MinGW-w64 GCC and `windres` on PATH: `.\build.bat`.
- Run `.\bin\winwm.exe` to replace Explorer for this session, or add `--preview` to run alongside it.
- Install for your account with `.\bin\winwm_install.exe /install`, then sign out and back in.
- Edit `%USERPROFILE%\.config\winwm\winwm.conf`. First launch creates it.

New app windows tile automatically on the selected workspace of their monitor. With
`follow_mouse = 1` in the `input` section, moving
the pointer into a tile gives it keyboard focus. Use Win+Shift+Arrow to move the
focused tile. Win+Shift+Space toggles floating.

## Mouse gestures

Hold **Win** and start the drag anywhere inside the window, including over a
terminal or another app's controls.

| Gesture | Tiled windows | Floating windows |
| --- | --- | --- |
| Win + left-drag | Drop on the highlighted center to swap places, including across monitors. Drop on an edge to insert beside the target. Drop on an empty monitor workspace to move there. Escape cancels a pending drop. | Move across monitors while keeping the grabbed point under the pointer. The window joins the visible workspace of the monitor containing the largest part of it. |
| Win + right-drag | Resize the available horizontal and vertical splits together. The side you grab determines which divider moves when there is a choice. | Drag from any quadrant to resize that corner. The opposite corner stays anchored and the app's tracking limits apply. |

Plain clicks continue to reach app controls. Dragging a shared tile divider also
resizes it when `resize_on_border = 1`. Tabbed and stacked layouts share their
tile area. Switch to a split layout for individual tiled resizing.
Floating windows stay visible over an overlapping tile when you focus that tile.

The `input` setting `floating_modifier = $mainMod normal` uses the default
[Sway button mapping](https://github.com/swaywm/sway/blob/master/sway/sway.5.scd).
Use `inverse` to swap the two buttons, or set a different modifier such as
`ALT normal`. Win+Shift+R reloads the configuration.

## Borders and motion

The focused window has a lavender-to-aqua outline. Inactive windows have a slate
outline. Drag the divider between tiles to resize them, or hold Win and right-drag
inside a tile. Fullscreen hides the outline. Winwm rounds standard Windows frames
using DWM, while apps with custom frames keep control of their own corners.

Right-click **Run** to open Settings. **Enable animations**, **Disable animations**,
**Follow Windows animation preference**, and **Show/Hide window borders** change
the running session. To keep a preference, edit `winwm.conf` and press Win+Shift+R.

```ini
general {
    border_size = 2
    resize_on_border = 1
    col.active_border = rgb(91aaf7) rgb(76daca) 45deg
    col.inactive_border = rgb(414656)
}
decoration {
    rounding = 8
}
animations {
    enabled = auto
    bezier = settle, 0.22, 1, 0.36, 1
    animation = windowsIn, 1, 1.6, settle, popin
    animation = windowsMove, 1, 1.9, settle
    animation = fadeIn, 1, 1.6, settle
    animation = fadeSwitch, 1, 1.4, settle
    animation = workspaces, 1, 1.8, settle, slide
    animation = border, 1, 1.4, settle
    animation = barModules, 1, 1.4, settle
    animation = layersIn, 1, 1.6, settle, fade
    animation = layersOut, 1, 1.2, settle, fade
}
```

Set `enabled` to `auto` to follow Windows, `1` to enable winwm motion, or `0`
to disable it. High Contrast suppresses motion in all three modes. Durations use
tenths of a second, so `1.6` means 160 ms. Each transition accepts `default`,
`linear`, or a previously declared Bézier curve. Both x coordinates range from
0 to 1. The y coordinates can range from −4 to 4 for overshoot.

| Effect | Controls |
| --- | --- |
| Tiled window entry | `popin 85%` starts at 85% of the final size. `slide` picks the nearest edge. To choose an edge, use `slide top`, `slide bottom`, `slide left`, or `slide right`. |
| Workspace entry | `slide 15%` and `slidevert 15%` move entering tiles horizontally or vertically by a fraction of their size. |
| Window opacity | `fadeIn` fades in tiled and floating windows. `fadeSwitch` eases focus changes to the configured inactive opacity. These effects apply to windows that allow winwm to control their opacity. |
| Toolbar controls | `barModules` eases hover and selected-workspace colors. `layersIn` and `layersOut` fade the bar and its volume, keyboard, and power flyouts. |

Use `animation = barModules, 0` or `animation = workspaces, 0` to disable an
individual effect. Mouse dragging follows the pointer directly. Application
closing uses the native Windows behavior.

Border width and corner radius use DPI-scaled units. A single color makes a solid
outline, and two colors plus an optional angle make a gradient. Set `border_size = 0`
to hide it. `winwm.exe ctl doctor` shows the effective motion setting and border width.
The configuration follows [Hyprland’s animation controls](https://wiki.hypr.land/0.54.0/configuring/animations/).

## Toolbar

| Control | Interaction |
| --- | --- |
| Speaker | Click and drag the volume slider, or use its arrow keys. Scroll the speaker in five-point steps. Right-click or middle-click to mute. The flyout also opens the per-app mixer and sound device chooser. |
| Keyboard (such as **EN**) | Click to choose a layout for the focused app. A check marks its current layout. Scroll to cycle layouts or right-click to open input setup. |
| Network and throughput | Click to see Wi-Fi networks or, on Ethernet-only systems, network adapters. Right-click for adapter settings. |
| Bluetooth | Click to inspect known devices or start pairing. |
| Battery or plug | Click for power status and **Keep awake**. Right-click for power settings. |
| Clock | Click to alternate between time and date. Right-click for date and time settings. |
| Run and window title | Click **Run** for the launcher or right-click for settings. Click the window title for the window switcher. Click a result once or use the arrow keys and Enter. |
| Workspaces, CPU, memory, and media | Click a workspace or scroll to switch. CPU and memory open the configured task manager. Media toggles playback. |

To add an input language, right-click the keyboard indicator, search by language
name or region code, select the result and choose **Add language**. To enable
a variant such as Dvorak, choose **Keyboard layouts**, search for it and choose
**Add keyboard**. **Set as default** also sets it as the preferred input layout.
If your custom bar omits the indicator, add `keyboard` to one of its module lists
or open **Add languages and keyboards** from the settings palette.

The volume gestures and input controls draw on
[Omarchy's bar configuration](https://github.com/omacom/omarchy/blob/master/config/waybar/config.jsonc),
[Waybar's volume slider](https://github.com/Alexays/Waybar/blob/master/man/waybar-pulseaudio-slider.5.scd)
and [Hyprland's layout switching](https://wiki.hypr.land/0.54.0/configuring/uncommon-tips--tricks/#switchable-keyboard-layouts).

## Verifying dependencies and behavior

`python tools/check_layers.py` checks the header and module dependency graphs,
the two executable source lists, and the embedded configuration. On a Windows
MinGW-w64 toolchain, `python tools/check_layers.py --linked` compiles both targets and
checks the actual cross-module symbol references against those boundaries.
CI also builds and runs the layout and configuration tests on Linux.
