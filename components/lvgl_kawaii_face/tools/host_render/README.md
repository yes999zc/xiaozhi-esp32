# Host (offline) render check for `lvgl_kawaii_face`

Renders the **real component source** with the **real LVGL** from this project on
the Mac/PC, at `LV_COLOR_DEPTH 16`, and dumps the three canvas buffers as raw
little-endian RGB565. No board, no flashing, no display required.

Use it to answer one question quickly: *does the component draw the right
pixels, or is the device path corrupting them?* (This is how the M7-S1
"green eyes / blue mouth" screenshot bug was traced to `SnapshotToJpeg`'s
byte swap rather than to the canvas code.)

## Run

```sh
cd components/lvgl_kawaii_face/tools/host_render
./run.sh                       # builds LVGL once (~1 min), then renders
python3 ../raw565_to_png.py out/*.raw     # eyeball the result
```

`run.sh` uses `LVGL_PATH` (defaults to
`$PWD/../../../../managed_components/lvgl__lvgl`) and writes objects into
`/tmp/facehost-obj` (outside the repo).

## Expected output

```
face_animation_init: 0
out/eye_left.raw: 144x144 cf=18 stride=288 data_size=41472
out/eye_right.raw: 144x144 cf=18 stride=288 data_size=41472
out/mouth.raw: 144x121 cf=18 stride=288 data_size=34848
```

`cf=18` is `LV_COLOR_FORMAT_RGB565`; the byte counts are `w*h*2` and must match
the device boot log (`canvas buffer 41472 B ...`, `34848 B`). The neutral face
must be: white sclera, **blue** iris, black pupil, **brown** brow, **crimson**
mouth. Anything else means the drawing code (not the transport) is wrong.

## Notes

* `lv_conf.h` here is host-only (CLIB malloc, no OS, no logs). Do not copy it
  into the firmware; the ESP build uses IDF Kconfig (`CONFIG_LV_*`).
* The panel size in `main.c` must stay in sync with `kFacePanelSize` in
  `main/boards/chd/chd-esp32s3-box/chd_esp32s3_box_board.cc` (currently 320).
* The component calls `memset()` without including `<string.h>`; on the host
  `run.sh` adds `-include string.h`. On ESP-IDF `lvgl.h` pulls it in.
