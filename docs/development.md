# Development

## Build and install

The default build uses `pkg-config hyprland`, including hyprpm's `PKG_CONFIG_PATH` when provided. Plugin builds do not require Python, bubblewrap, Wayland scanner or the test helpers.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j2
cmake --install build
```

Only `hyprauto.so` is installed, under `${CMAKE_INSTALL_LIBDIR}/hyprland`. `DESTDIR` is supported for staging. The `uninstall` target removes files listed in that build directory's installation manifest, including a supplied `DESTDIR`:

```sh
DESTDIR="$PWD/build/stage" cmake --install build
DESTDIR="$PWD/build/stage" cmake --build build --target uninstall
```

The Makefile wraps these commands with a user-local default prefix. `BUILD_DIR`, `PREFIX` and `CMAKE_ARGS` are configurable. The `hyprpm.toml` manifest builds only the plugin, without running installation, loading or tests. A Git repository containing committed project sources is required before using it with `hyprpm add`; the project does not assume a published remote URL.

For a different configured Hyprland checkout, use a separate build directory:

```sh
cmake -S . -B build-source -DCMAKE_BUILD_TYPE=Release -DHYPRLAND_SOURCE=/path/to/Hyprland
cmake --build build-source -j2
```

That checkout must provide the matching generated version and protocol headers. The build uses that checkout's headers instead of installed Hyprland headers; dependency packages still come from pkg-config. Use the same compiler as the target compositor. Do not install a build from one revision into a compositor running another revision.

`src/HyprlandCompat.hpp` isolates window metadata and IPC registration differences between Hyprland 0.56.2 and the newer window/socket API. It does not provide runtime ABI compatibility or a promise of support for arbitrary revisions.

## Headless integration tests

Tests are opt-in and require Python 3, `nm`, `bwrap`, Wayland protocols/scanner, a DRM render node, and a matching Hyprland checkout containing the hyprtester plugin source.

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release \
    -DHYPRLAND_SOURCE=/path/to/Hyprland \
    -DHYPRAUTO_BUILD_TESTS=ON \
    -DHYPRAUTO_TEST_BINARY=/path/to/Hyprland/build/Hyprland
cmake --build build-tests -j2
ctest --test-dir build-tests --output-on-failure
```

`HYPRAUTO_TEST_BINARY` defaults to `${HYPRLAND_SOURCE}/build/Hyprland`. The runner supports testing an independently built or installed plugin against the same compositor revision:

```sh
python3 tests/test.py --binary /path/to/Hyprland \
    --build-dir build-tests --plugin "$HOME/.local/lib/hyprland/hyprauto.so"
```

Logs are saved under the selected build directory's `test-artifacts/`. `make test BUILD_DIR=build-tests` runs CTest and fails if no tests are configured. Test helpers are never installed.

### Isolation

The harness creates fresh runtime/config/cache directories, removes inherited desktop and DBus connection variables, and disables systemd environment and target integration. IPC requests connect directly to the isolated instance.

Bubblewrap gives the compositor a private PID namespace, `/dev` and `/run`, with a read-only filesystem view. Only one DRM render node is exposed; KMS card nodes, physical input devices and host session sockets are not exposed.

`tests/headless.cpp` is a test-only preload shim. It restricts Aquamarine to the mandatory headless backend and provides a render-node FD for the native GBM allocator and Hyprland OpenGL renderer. It does not replace rendering or input delivery and must not be used in a desktop session. The runner also asserts that every output is headless.

`tests/test-support.cpp` reuses the matching hyprtester plugin's synthetic devices and Lua helpers, then updates seat capabilities. `tests/client.c` is a SHM Wayland toplevel recording activation, enter/leave, input events and keysyms from the received keymap. A stdin roundtrip barrier makes assertions independent of arbitrary sleeps.

### Checks

1. Loading hooks without a session preserves normal keyboard, pointer and scroll input.
2. One background session enter without host focus, cursor or target activation changes.
3. Held keys/buttons survive host focus changes; input to other clients stays separate.
4. Automation modifiers do not inherit host modifiers.
5. The session keymap remains stable across host layout changes and is restored on end.
6. Default coexistence delivers host keys, modifiers, motion, buttons and scroll without overwriting automation state, including overlapping key codes.
7. Injection reapplies automation modifiers and pointer position after host input.
8. Native cursor routing accepts foreground requests and rejects background requests; the target receives its final pointer frame on host departure.
9. Blocking suppresses target host events, not automation or input to other clients; focus changes do not send session leave/re-enter events.
10. Unblocking resumes delivery without restarting the session; end clears the policy.
11. Ending a blocked foreground session restores ordinary host routing.
12. Invalid input, malformed block modes, unmatched releases, duplicate presses and out-of-bounds coordinates are rejected; ending releases held inputs.
13. Target unmap releases inputs and cancels the session.
14. Target disconnect cancels the session without breaking compositor or host input.
15. A background target can be cropped to a PNG without changing host focus; the image dimensions and pixel content match the target surface.
16. Unload/reload restores ordinary routing; sessions can begin on a foreground target with coexistence enabled.

The suite has been validated against Hyprland 0.56.2 (`efb50993780079460b0cbed1363e2166a2de1d9f`) and `41a5d15ac07b3729eda6d26e629f027ab69ed4d9`, using GCC, Aquamarine 0.15.1 and Hyprutils 0.14.2. These are protocol-level tests, not application compatibility tests.

## Input sessions

`src/main.cpp` finds mapped native Wayland windows through `CViewQuery`, discovers their existing seat resources using Wayland's resource iterator, and holds weak references to their keyboard/pointer resources.

A session owns pressed-key/button sets, surface-local pointer coordinates and an independent `xkb_state` initialized from the active keyboard's keymap. The keymap is a snapshot for the session lifetime. Host input does not update these automation models. Before injecting a key, the session reapplies its modifiers; before injecting a button, it sends its stored pointer position.

Each session starts with `block_input: false`. `hyprauto block-input on|off` changes delivery policy during an active session without restarting it. End/unmap/disconnect reset the policy. A target may already have host focus when the session begins.

`hyprauto screenshot <path>` renders a temporary standalone window snapshot, reads back only the target main-surface rectangle, and writes that crop as a PNG. The render hook forces Hyprland's standalone mode for this synchronous call, excluding window opacity and decorations while preserving alpha from the client surface. Capture fails if the window is not currently renderable. The temporary readback buffer is bounded to 64 megapixels.

Delivery uses Hyprland's `CWLKeyboardResource` and `CWLPointerResource` methods. They retain responsibility for serials, button serial tracking, resource lifetime listeners, fixed-point coordinates, capability checks and protocol events. No custom Wayland keyboard/pointer implementation or seat-focus swap is introduced.

Private hooks control compositor-originated events for the automation client:

- Keyboard/pointer enter and leave are suppressed during the session to retain the target surface's protocol focus.
- Keys, modifiers, motion, buttons and axis events are forwarded by default while the corresponding host focus belongs to the target client. This also excludes compositor-wide modifier broadcasts while the target is in the background. Frames are allowed after pointer departure to flush events delivered before the focus change.
- `block_input` suppresses those host input events, not synchronous automation dispatch.
- Keymap and repeat-info broadcasts exclude the target, leaving the session snapshot intact.

Other clients use the original functions. Cursor requests use Hyprland's existing foreground-client checks, with no cursor hook. The session does not write compositor window focus, seat focus, physical pressed-key state or global pointer coordinates.

This is client-local protocol focus on the existing seat, not a second advertised seat. A background target receives enter while remaining unactivated. Host focus changes do not end its protocol focus or pause automation. Manual and automated events share the client's resources; no application-side state isolation is promised. Input for another window on the same client connection can reach the pinned target surface. Overlapping presses/releases can affect application state, and the block switch does not replay physical input state.

End/unload releases held inputs, clears modifiers, sends leave/frame and restores the current host keymap/repeat settings. If host focus is on the target client, ordinary input routing is re-entered. Unmap/destruction cancels without re-entering closing surfaces.

The single-session bypass flag assumes synchronous, single-threaded input dispatch. Concurrent sessions need per-client routing state. Session setup snapshots bound input resources; dynamic rebinding needs additional lifecycle handling. Popup/subsurface targeting, XWayland, IME, pointer constraints, relative-pointer delivery, drag-and-drop, touch and tablet input are outside the current scope. The block policy covers the hooked `wl_keyboard`/`wl_pointer` events, not these additional input protocols. Screenshot output is the standalone-rendered main-surface crop; it does not include the monitor or host cursor, and it ignores Hyprland's window opacity.

## Backend choice

Target-specific background delivery uses existing Wayland resources. Hyprland input-capture exports captured host events through EIS; its `CEis::onEvent` rejects sender clients. It is not an injection route into an arbitrary background Wayland client. Introducing libei/libeis would still require target routing and focus handling, so this delivery policy does not need a new backend.

## Hook safety

The plugin checks the Hyprland commit hash before installing hooks. Each lookup requires exactly one matching demangled method and successful trampoline installation. Hyprland's plugin loader removes registered hooks if initialization fails.

These checks do not establish ABI stability. Rebuild and rerun the integration suite after compositor, compiler or dependency upgrades. No Hyprland source modifications are required.
