# Unit LCD-Plus UI preview

## Current design (2026-09-09)

Boot restores the original M5STACK logo (100x100, centered on white) for 1000ms,
then enters Home without a page translation. Current I2C address appears above the
logo, with the firmware version below; both use the same live input as Home/Device.
The release uses FIRMWARE_MAJOR_VERSION and FIRMWARE_MINOR_VERSION from
src/common.hpp (currently 1.1), also used by READ_ID; UI formats both as V1.1. It uses an LVGL RGB565 image in Flash,
converted from the original MIT-licensed src/logo.hpp artwork. The UI timer drives
the transition without delaying I2C processing; host ownership may preempt the logo.
Inputs received during the logo are ignored and cannot activate a menu on release.
The legacy textual splash is retained only for old ESP32 or LVGL initialization failure.

ESP32-C61, ST7789 135x240 portrait display, encoder and GPIO9 button.
The firmware and SDL simulator share `ui/unit_lcd_ui.cpp`. M5GFX remains the
transport layer; the existing I2C display ownership and address-save paths are unchanged.

- Home: centered `Unit LCD-Plus`, centered current I2C address, no host-wait text.
  Menu order: Address, Theme, Device. Both palettes use a full-width rectangular
  address band (orange in Light, dark teal in Dark).
- Address: SETTING / I2C Address; a 48px-high selectable address frame and a
  bottom Back row. Initially rotate between the address frame and Back, without
  changing the value. Click the frame to begin adjustment (green text and thicker theme-colored border);
  rotate within 0x08..0x77, then click to confirm. No instructional hint text is
  shown. Six equal-width 24px slots center <0xXX>; all characters use the same color.
  Light editing green is #238A55; Dark is #49D17D. Back discards the pending edit.
  Valid Range and Current groups are 10px higher; Back remains at y=202.
- Confirm: old address -> new address, `Applying` / `will reboot!` on two centered red 14px lines at y=110, Apply / Cancel.
  Entering this page does not save. Cancel returns to address selection with the
  pending value retained. The warning is centered red text without a frame, at y=123.
- Theme: SETTING / Theme; two permanent 50px preview cards at y=72 and y=134,
  each with a palette swatch, name and applied-theme check. The moving outer border
  indicates focus independently from the check. Light (former Utility, persisted ID 1) and Dark
  (former Precision, ID 0). Selection applies immediately and requests persistence.
  A third Back row returns home. Legacy Console (ID 2) falls back to Light.
  The stored enum IDs are not renumbered.
- Device: three sections: SoC ESP32-C61 + Flash 4MiB; Display 135x240;
  Firmware V<actual firmware version>. Back returns focus to Device.
- Long press has no navigation action; it disarms the subsequent release click.
  All returns use visible Back/Cancel rows. Returning from Theme restores focus to Theme.

Both themes use the same x=7, width=121 content bounds. Bottom Back/Cancel rows
start at y=202 with height 34; confirmation and failure actions use y=160/202.
Confirmation addresses share a baseline and symmetric columns around the center.

The header selects one common font from the longest title Unit LCD-Plus within
123px and the 30px header height. All pages use that font and the same vertical
position. Whole-page translation is removed. Selector and address-digit springs
remain; no new animation dependency or framebuffer is added.
The address uses six 24px equal-width slots including angle brackets. A static
fallback uses identical slots and colors if the rolling pair cannot be allocated.

## Build and run

Prerequisites: CMake >=3.20, Ninja, C/C++ compiler, SDL2 development files,
PlatformIO and the existing ESP32-C61 toolchain for firmware builds.

The validation below uses the installed firmware dependency LVGL 9.5.0 for the
simulator as well. The CMake fallback remains pinned to LVGL 9.3.0; explicitly
supply the 9.5.0 source for the validated configuration:

```bash
pio pkg install -e release
cmake -S simulator -B .build/ui-preview -G Ninja \
  -DFETCHCONTENT_SOURCE_DIR_LVGL="$PWD/.pio/libdeps/release/lvgl"
cmake --build .build/ui-preview -j 4
.build/ui-preview/unit-lcd-ui-sim --variant light
.build/ui-preview/unit-lcd-ui-sim --variant dark --state edit
pio run -e release
```

Controls: Up/Left or wheel up = previous; Down/Right or wheel down = next;
Enter/Space or left mouse press/release = click; Esc/Backspace = simulate an ignored long hold;
Q or window close = exit. Restart the program to reset simulated address/theme.
`--variant light|dark` selects the initial theme. Legacy precision/utility aliases
remain supported; console resolves to Light.

States: boot, home, pressed, edit (selection), adjust (editing), confirm, saving, saved, error, about, theme.
`--firmware-version 0..127` injects the minor version for Device checks (default 1; major 1).
`--smoke` runs interaction regression; `--fail-save` simulates persistence failure;
`--stats` reports LVGL heap. The simulator never writes device NVS or reboots hardware.

```bash
mkdir -p .build/ui-preview/captures
SDL_VIDEODRIVER=offscreen .build/ui-preview/unit-lcd-ui-sim \
  --variant light --state home --capture .build/ui-preview/captures/light-home.bmp
SDL_VIDEODRIVER=offscreen .build/ui-preview/unit-lcd-ui-sim --variant dark --smoke
SDL_VIDEODRIVER=offscreen .build/ui-preview/unit-lcd-ui-sim --variant light --smoke --fail-save
```

Use a normal desktop SDL window for interactive operation. `offscreen` was available
on this Linux host and generated actual 135x240 SDL renderer captures; screenshots
are desktop evidence, not physical-panel evidence. The helper run_ui_preview.sh still
supports a source override via FETCHCONTENT_SOURCE_DIR_LVGL; direct commands above
avoid its headless dummy-driver default when rendering screenshots.

## Validation for this revision

- PASS: Light/Dark x 11 states: 22 screenshots with layout validation.
- PASS: 5 interaction smoke runs: both themes with success/failure and legacy
  Console with V127. Includes a 600ms visible-logo check, transition after 1000ms,
  ignored boot input, and all prior navigation, bounds, Cancel/Back and retry cases.
- PASS: each click checks header font/y, content origin and divider at entry,
  80ms and 200ms. There is no page translation.
- PASS: equal character-slot width/position, centered group and uniform colors in
  normal/editing states, including the rolling digit columns and boundary values.
- Previous revision PASS: separate static-fallback simulator build and smoke. Its Light/Dark edit/adjust
  captures are pixel-identical to the rolling version (4 image comparisons).
- PASS: ESP32-C61 release build: RAM 66,764 B (20.4%), application Flash 733,556 B (56.0%).
- Simulator LVGL heap peak: 20,896 B; not a hardware heap measurement.
- Physical panel/color, real input timing, NVS power-cycle persistence and
  address-change reboot remain unverified by these desktop tests.

To reproduce static fallback checks, use a separate simulator build directory:

```bash
cmake -S simulator -B .build/ui-preview-static -G Ninja \
  -DFETCHCONTENT_SOURCE_DIR_LVGL="$PWD/.pio/libdeps/release/lvgl" \
  -DCMAKE_CXX_FLAGS=-DUNIT_LCD_UI_TEST_STATIC_ADDRESS
cmake --build .build/ui-preview-static -j 4
SDL_VIDEODRIVER=offscreen .build/ui-preview-static/unit-lcd-ui-sim --variant dark --smoke
```

The macro is a test-only path; firmware builds do not define it.

I2C ownership rules remain: only an accepted host display transaction takes the
screen; read/probe traffic does not. IAP/reset retain their priority. UI rendering
stays in its owner task; local encoder input does not consume the host event queue.
Address changes reuse existing validation, NVS commit and restart logic, including
existing rejection of the current address. This revision does not change that policy.

Default backlight is 204/255 (80%). The page title is Device Info; the Home menu remains Device.
