# Local patches vs upstream `lvgl_kawaii_face`

Upstream: <https://github.com/0015/lvgl_kawaii_face> (MIT, see `LICENSE`).
Vendored at commit state of 2026-10-27 into `components/lvgl_kawaii_face/`.

`main/boards/chd/chd-esp32s3-box/chd_esp32s3_box_board.cc` is the only consumer
(M7-S1 "persona face" spike). Everything below is the complete delta against
upstream — `diff` it against a fresh clone to verify.

## 1. `lvgl_kawaii_face.c` — canvas buffers prefer PSRAM instead of internal RAM

Upstream `face_malloc_canvas()` tried `MALLOC_CAP_INTERNAL` first and only fell
back to `MALLOC_CAP_SPIRAM`. The three RGB565 canvases are large (tens of KiB on
this panel geometry) and are only touched by the CPU (LVGL software renderer),
so taking them from DRAM competes with Wi-Fi / AFE / WakeNet, which this project
reserves ~100 KiB of internal RAM for (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`).

Patched order: `MALLOC_CAP_SPIRAM` → `MALLOC_CAP_INTERNAL` → libc `malloc`.
Behaviour is unchanged when PSRAM is absent (`CONFIG_SPIRAM` unset).

## 2. `lvgl_kawaii_face.c` — one log line per canvas buffer (evidence only)

`face_malloc_canvas()` logs `size → pointer (PSRAM|internal RAM|libc fallback)`.
This is what the M7-S1 acceptance criterion for "canvas memory placement" is
verified against in the boot log; it has no functional effect.

## 2b. Patched in task-11 — canvas buffers now use exact RGB565 size

Upstream sizes the buffers as `w * h * sizeof(lv_color_t)`. In LVGL 9
`lv_color_t` is `struct { uint8_t b, g, r; }` — **3 B/px, independent of
`LV_COLOR_DEPTH`** — while an `LV_COLOR_FORMAT_RGB565` canvas only needs
**2 B/px** (`stride = w * 2`). So upstream allocates 1.5x (33 % more) than the
canvas actually uses; `lv_draw_buf_init()` accepts the larger buffer silently.

For the current geometry (S=320, eyes 144x144 x2, mouth 144x121):

| | bytes | |
|---|---|---|
| allocated by upstream formula (3 B/px) | 176,688 | ~172.5 KiB |
| actually needed by RGB565 (2 B/px) | 117,792 | ~115 KiB |
| wasted | 58,896 | allocation is 1.5x the size the canvas needs (+50 %) |

**Patched in M7-S1 rev4 (task-11).** The buffers are now sized exactly like the
canvas image (`LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565)` = 2 B/px), so
the boot log reports `eye 144x144=41472 B x2, mouth 144x121=34848 B`
(total 117,792 B instead of 176,688 B) and the two numbers can no longer drift
apart from `lv_draw_buf_init()`'s `data_size = stride * h`.

## 2c. Rendering investigation (M7-S1, 2026-10-27) - colours in *screenshots* are byte-swapped

Measured on the rev3 device screenshots (`esp32-voice-entry/docs/face/step1-*.jpg`):

| element | design colour (component) | host render of this file | device screenshot |
|---|---|---|---|
| iris | `lv_color_make(50,180,255)` | (49,182,255) | (183,226,170) |
| eyebrow | `lv_color_make(80,60,40)` | (82,61,41) | (230,170,150) |
| mouth | `(190,60,80)` @ OPA_90 | (189,77,90) | (103,115,208) |
| sclera / pupil | white / black | (255,255,255) / (0,0,0) | unchanged |

Every saturated colour in the screenshot equals the rendered colour with its two
RGB565 bytes exchanged (`__builtin_bswap16`); white and black are invariant,
which is why only the face looked wrong. Verified by rendering this component
headless with LVGL 9.5 at `LV_COLOR_DEPTH 16` (host build): the canvas content is
correct (blue iris, brown brow, crimson mouth, stride 288 / data_size 41,472).

Cause is **not** in this component and **not** in the canvas format: it is the
unconditional byte swap in `main/display/lvgl_display/lvgl_display.cc`
(`LvglDisplay::SnapshotToJpeg`, "swap bytes" loop) applied to an
`lv_snapshot_take(..., LV_COLOR_FORMAT_RGB565)` buffer that is already native
little-endian. The display colour format stays `LV_COLOR_FORMAT_RGB565`
(`esp_lvgl_port_disp.c:306-320`); `swap_bytes` is only a flush-time flag
(`esp_lvgl_port_disp.c:341`), so nothing in the snapshot path needs swapping.

Consequence: the *panel* is expected to be correct (it gets exactly one swap in
the flush callback), while every `/screen/snapshot` JPEG had wrong colours.

**Fixed in rev2 (task-11, Lead-approved scope):** the `__builtin_bswap16` loop in
`LvglDisplay::SnapshotToJpeg` (`main/display/lvgl_display/lvgl_display.cc`) is
gone; the native RGB565 snapshot is handed straight to the JPEG encoder. Live
display is unaffected - the panel byte order is still applied once per flush
(`esp_lvgl_port_disp.c:739-742`). This was a pre-existing bug affecting every
coloured snapshot, not just the face.

## 3. `CMakeLists.txt` — project-local wiring

* Kept `REQUIRES lvgl__lvgl` (managed-component name in this project).
* Dropped the optional `espressif__esp_lvgl_port` link block; the board injects
  the LVGL port lock/unlock callbacks via `face_set_lvgl_lock_fns()`.
* Dropped the unused `esp_timer` private requirement.
* No `idf_component.yml` is shipped on purpose: this is a `components/`-local
  component, not a registry dependency, and a manifest here would make the
  component manager re-resolve `lvgl/lvgl`.
