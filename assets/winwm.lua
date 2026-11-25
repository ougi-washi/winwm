local browser = "brave"
local mod = "SUPER + "
local shift = "SUPER + SHIFT + "
local dsp = hl.dsp
local repeat_key = { repeating = true }

hl.config({
    general = {
        layout = "dwindle", gaps_in = 2, gaps_out = 2, border_size = 1,
        resize_on_border = true,
        col = {
            active_border = { colors = { "rgb(91aaf7)", "rgb(76daca)" }, angle = 45 },
            inactive_border = "rgb(414656)",
        },
    },
    decoration = {
        rounding = 2, inactive_opacity = 0.95,
        panels = { button_size = 32, spacing = 6, opacity = 0.94, blur = true, shadow = true },
    },
    animations = { enabled = "auto" },
    notifications = {
        enabled = true, capture = "windows", corner = "top_right",
        duration_ms = 5000, fade_ms = 160, history_limit = 128,
        visible_limit = 3, width = 360, margin = 12,
    },
    input = { follow_mouse = 1 },
    bar = {
        position = "top", height = 32, workspace_count = 5,
        modules_left = { "menu", "workspaces", "mode" },
        modules_center = { "clock" },
        modules_right = { "tray", "cpu", "bluetooth", "network", "audio", "keyboard", "battery", "notifications" },
        tooltip_delay = 400, tooltip_duration = 4500,
        background = "rgb(101116)", foreground = "rgb(e4e5ea)",
    },
    programs = {
        terminal = "auto", file_browser = "FilePilot.exe", task_manager = "TaskSlinger.exe",
        editor = [[%SystemRoot%\System32\notepad.exe]],
    },
})

hl.curve("settle", { type = "bezier", points = { { 0.22, 1 }, { 0.36, 1 } } })
for _, motion in ipairs({
    { "windowsIn", 1.6, "popin" }, { "windowsMove", 1.9 },
    { "fadeIn", 1.6 }, { "fadeSwitch", 1.4 }, { "workspaces", 1.8, "slide" },
    { "border", 1.4 }, { "barModules", 1.4 }, { "layersIn", 1.6, "fade" },
    { "layersOut", 1.2, "fade" },
}) do
    hl.animation({ leaf = motion[1], enabled = true, speed = motion[2], bezier = "settle", style = motion[3] })
end

hl.bind(mod .. "mouse:272", dsp.window.drag(), { mouse = true })
hl.bind(mod .. "mouse:273", dsp.window.resize(), { mouse = true })
hl.bind(mod .. "Return", dsp.winwm("terminal"))
hl.bind(shift .. "Q", dsp.window.close())
hl.bind(mod .. "D", dsp.winwm("menu"))
hl.bind(mod .. "B", dsp.exec_cmd(browser))
hl.bind(shift .. "T", dsp.winwm("open_tasks"))
hl.bind(shift .. "R", dsp.winwm("reload"))
hl.bind(shift .. "E", dsp.winwm("confirm_sign_out"))
hl.bind(mod .. "Space", dsp.winwm("focus_mode_toggle"))
hl.bind(mod .. "E", dsp.layout("togglesplit"))
hl.bind("Print", dsp.winwm("capture_region"))
hl.bind(mod .. "F6", dsp.winwm("capture_region"))
hl.bind("XF86AudioMute", dsp.winwm("mute"))
hl.bind("XF86AudioRaiseVolume", dsp.winwm("volume_up"), repeat_key)
hl.bind("XF86AudioLowerVolume", dsp.winwm("volume_down"), repeat_key)
hl.bind(mod .. "H", dsp.winwm("split horizontal"))
hl.bind(mod .. "V", dsp.winwm("split vertical"))
hl.bind(mod .. "S", dsp.winwm("layout stack"))
hl.bind(mod .. "W", dsp.winwm("layout tabs"))
hl.bind(mod .. "F", dsp.window.fullscreen())
hl.bind(shift .. "Space", dsp.window.float())
hl.bind(mod .. "Minus", dsp.winwm("scratch_show"))
hl.bind(shift .. "Minus", dsp.winwm("scratch_send"))
hl.bind(mod .. "R", dsp.submap("resize"))
hl.bind("CTRL + ALT + Space", dsp.winwm("menu"))
hl.bind("CTRL + Escape", dsp.winwm("menu"))
hl.bind(mod .. "P", dsp.winwm("open_folder"))
hl.bind(shift .. "Escape", dsp.winwm("open_tasks"))
hl.bind("CTRL + SHIFT + Escape", dsp.winwm("open_tasks"))
for _, chord in ipairs({ mod .. "Tab", "ALT + Tab", "ALT + SHIFT + Tab", "ALT + Escape" }) do
    hl.bind(chord, dsp.winwm("switcher"))
end
hl.bind("ALT + F4", dsp.window.close())
hl.bind(shift .. "V", dsp.winwm("split horizontal"))
hl.bind(shift .. "C", dsp.winwm("reload"))
hl.bind(shift .. "P", dsp.winwm("settings"))
hl.bind(shift .. "I", dsp.winwm("clipboard"))
hl.bind(shift .. "X", dsp.winwm("capture"))
hl.bind(shift .. "B", dsp.winwm("bar_toggle"))
hl.bind(shift .. "D", dsp.winwm("output_send"))
hl.bind(mod .. "O", dsp.winwm("output_next"))

local directions = {
    { "Left", "H", "left", -100, 0 }, { "Down", "J", "down", 0, 100 },
    { "Up", "K", "up", 0, -100 }, { "Right", "L", "right", 100, 0 },
}
for _, d in ipairs(directions) do
    hl.bind(mod .. d[1], dsp.focus({ direction = d[3] }))
    hl.bind("CTRL + ALT + " .. d[1], dsp.focus({ direction = d[3] }))
    if d[2] ~= "H" then hl.bind(mod .. d[2], dsp.focus({ direction = d[3] })) end
    hl.bind(shift .. d[1], dsp.window.move({ direction = d[3] }), repeat_key)
    hl.bind(shift .. d[2], dsp.window.move({ direction = d[3] }), repeat_key)
end
for i = 1, 10 do
    hl.bind(mod .. (i % 10), dsp.focus({ workspace = i }))
    hl.bind(shift .. (i % 10), dsp.window.move({ workspace = i, follow = false }))
end
hl.define_submap("resize", function()
    for _, d in ipairs(directions) do
        local resize = dsp.window.resize({ x = d[4], y = d[5], relative = true })
        hl.bind(d[1], resize, repeat_key)
        hl.bind(d[2], resize, repeat_key)
    end
    hl.bind("Return", dsp.submap("reset"))
    hl.bind("Escape", dsp.submap("reset"))
end)
