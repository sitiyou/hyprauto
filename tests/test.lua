hl.monitor({ output = "HEADLESS-1", mode = "1280x720@60", position = "0x0", scale = "1" })
hl.config({
    animations = { enabled = false },
    debug = { enable_stdout_logs = true },
    decoration = { blur = { enabled = false }, dim_inactive = true, dim_strength = 0.5, shadow = { enabled = false } },
    input = { follow_mouse = 1 },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
})
hl.window_rule({ name = "test-windows", match = { class = "^(target|host-a|host-b)$" }, float = true, size = "320 240" })
hl.window_rule({ name = "target-position", match = { class = "^target$" }, move = "30 30", opacity = "0.35 0.35" })
hl.window_rule({ name = "host-a-position", match = { class = "^host-a$" }, move = "430 30" })
hl.window_rule({ name = "host-b-position", match = { class = "^host-b$" }, move = "830 30" })
