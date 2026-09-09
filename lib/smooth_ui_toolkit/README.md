# smooth_ui_toolkit (vendored core subset)

Upstream: https://github.com/Forairaaaaa/smooth_ui_toolkit
Pinned release: `v2.14.0` (commit `3cfe6a373edee2e325b8490491f921ecc71b29e6`), MIT (see `LICENSE`).

Only `src/core` is vendored — the parts this firmware uses:

- `animation/animate_value`: retargetable spring/easing scalar (selector block, hex digit roll)
- `animation/generators`, `easing`: spring and easing keyframe generators
- `hal`: tick source; the firmware overrides `ui_hal::on_get_tick` with `millis()`

The LVGL widgets (`src/lvgl`, `NumberFlow`/`DigitFlow`), selector menus (`src/widget`),
games, and tools are intentionally not vendored: the address readout needs a hex
charset that upstream `DigitFlow` does not support, so `ui/hex_digit_flow.hpp`
re-implements the same wraparound-column technique on top of `AnimateValue`.
The upstream repo is also not consumed via `lib_deps` because its `library.json`
sets `srcDir: "."`, which would compile the raylib-based `example/` programs.

Upgrade: replace `src/core` and `LICENSE` with the new tag's copies and record the
new tag/commit here.
