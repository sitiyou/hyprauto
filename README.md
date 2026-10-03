# hyprauto

A Hyprland plugin for sending keyboard and pointer input to a selected native Wayland toplevel. Automation sessions are independent of host focus and do not move the host cursor.

Automation maintains its own key/button state, modifiers, keyboard layout, and pointer coordinates. By default, user input and automation input can coexist. A session can optionally block user input to its target client. Host focus changes do not interrupt the session; ending the session, closing the target, or unloading the plugin releases automation input.

> This plugin runs inside the compositor and uses private Hyprland hooks. Build it against the headers for the running Hyprland revision and test it in an isolated instance first. Application compatibility is not guaranteed.

## Installation

Install and enable the plugin with [hyprpm](https://wiki.hypr.land/Plugins/Using-Plugins/):

```sh
hyprpm add https://github.com/sitiyou/hyprauto.git
hyprpm enable hyprauto
```

The plugin uses private Hyprland hooks, so hyprpm must build it against headers matching your Hyprland revision. After upgrading Hyprland, run `hyprpm update` to rebuild plugins.

To uninstall:

```sh
hyprpm disable hyprauto
hyprpm remove https://github.com/sitiyou/hyprauto.git
```

When using an isolated instance, pass `hyprctl --instance "$INSTANCE"` and use that instance's `XDG_RUNTIME_DIR`.

## Usage

Select a native Wayland client with bound keyboard and pointer resources. It can be in the foreground or background; starting a session does not change host focus.

```sh
hyprctl hyprauto begin class:target
hyprctl hyprauto move 40 50
hyprctl hyprauto key 42 down
hyprctl hyprauto key 30 down
hyprctl hyprauto key 30 up
hyprctl hyprauto key 42 up
hyprctl hyprauto button 272 down
hyprctl hyprauto button 272 up
hyprctl hyprauto screenshot "$HOME/Pictures/target.png"
hyprctl hyprauto status
hyprctl hyprauto end
```

| Command | Description |
| --- | --- |
| `begin <selector>` | Start a session using a Hyprland window selector, such as `class:target` or `address:0x...`. |
| `key <code> down\|up` | Press or release a Linux evdev key code. |
| `move <x> <y>` | Move the automation pointer in coordinates relative to the target's main surface. |
| `button <code> down\|up` | Press or release a mouse button. |
| `screenshot <path>` | Save the target's rendered main surface as a PNG; quote paths containing spaces. |
| `block-input on\|off` | Block or allow user keyboard and pointer events to the target during an active session. |
| `status` | Return session and host input state as JSON, including `block_input`. |
| `end` | Release automation input and end the session; safe to call repeatedly. |

`42` is left Shift, `30` is A, and `272` is the left mouse button. Movement coordinates must be inside the target surface. Commands return `ok` or `error: ...`, except `status`. Duplicate presses, unmatched releases, and invalid arguments are rejected.

### User and automation input

Each session starts with `block_input: false`. When the user interacts with the target client, keyboard, modifier, pointer-motion, button, and scroll events are delivered normally. Automation continues while the user visits or leaves the target. Before injecting a key or button, automation reapplies its own modifiers or pointer position.

To block user input:

```sh
hyprctl hyprauto block-input on
hyprctl hyprauto block-input off
```

Blocking filters only user events sent to the target client's `wl_keyboard` and `wl_pointer` resources. It does not affect automation, other clients, host focus, physical cursor movement, or Hyprland key bindings. The policy resets when the session ends.

A screenshot requires an active session and a target currently renderable by Hyprland. The PNG contains the target main-surface crop, not the full monitor or host cursor. Hyprland window opacity and decorations are excluded; alpha in the client surface itself is preserved.

User and automation events share the target client's protocol resources; application-level state isolation is not guaranteed. Overlapping presses, releases, and interleaved events may affect the application, and users are responsible for the consequences. Toggling the block policy does not replay physical input state; release held user keys and buttons before changing it.

## Uninstallation

Remove any startup-load configuration, then run:

```sh
hyprctl plugin unload "$HOME/.local/lib/hyprland/hyprauto.so"
make uninstall
```

## Scope

- One session at a time; native Wayland toplevels; coordinates relative to the main surface.
- The target main surface retains protocol focus on all bound keyboard and pointer resources. This is not a second seat and does not isolate multiple windows belonging to the same client. User input for another window on that client connection may reach the automation target.
- XWayland, IME, popup/subsurface hit-testing, pointer-lock/relative-pointer, touch/tablet, scroll injection, drag-and-drop, and resource rebinding during a session are not supported.
- Protocol behavior is tested with the included client. Compatibility with GTK, Qt, browsers, and games requires separate testing.

## Python controller

`hyprauto_ctrl` provides an asynchronous Python controller using the same core method names as MaaFramework. Pass `target` at construction or change the selected window with `set_target()`. Target changes replace the current session. `post_connection(instance=...)` can select a Hyprland instance; when omitted, it uses `HYPRLAND_INSTANCE_SIGNATURE` from the environment. The runtime socket is resolved under `XDG_RUNTIME_DIR`.

```python
from hyprauto_ctrl import HyprAutoController

controller = HyprAutoController(target="class:target")
controller.post_connection().wait()
controller.post_click(40, 50).wait()
controller.post_click_key(30).wait()
image = controller.post_screencap().wait().get()
controller.set_target("address:0x...").wait()
controller.close()
```

Screenshots are returned as NumPy BGR images and the latest capture is available from `controller.cached_image`. Jobs expose `succeeded` and `error`. The plugin currently permits one active session per Hyprland instance, so selecting another target switches the active session; it does not create simultaneous sessions. `close()` ends the active session without unloading the plugin.

The project includes `hyprpm.toml` for building and managing the plugin with hyprpm. See the [development guide](docs/development.md) for build, isolated headless testing, and implementation details.
