# pocketjs_ui_qjs

QuickJS binding for the PocketJS `ui` HostOps surface.

- Public header: `pocketjs/ui_qjs.h`
- Host: RT-Thread; first consuming port is ArtInChip D13x
- Dependencies: `pocketjs_guest`, `pocketjs_ui_core`

The binding borrows a guest and core and derives its viewport and tick rate
from that core. Feed the target PAK, mount the binding, then evaluate the JS
bundle. `pocketjs_ui_turn` resolves touch-hit facts, runs one guest turn,
advances the core, and returns a frame view. **It does not render, present, or
create a task.** Destroy the guest before the mounted binding.

Derived from the ESP-IDF reference with RT-Thread error values and allocation
through the guest host-memory seam. PAK constants are generated from the shared
contracts. Framebuffer presentation and JS thread ownership belong to the app.
