#!/bin/sh
# Build and run the host (offline) face renderer. See README.md.
# Usage: ./run.sh              (LVGL from this project's managed_components)
#        LVGL_PATH=/path ./run.sh
set -e
# md5 is the macOS name; Linux uses md5sum. Pick whichever exists.
if command -v md5sum >/dev/null 2>&1; then MD5=md5sum; else MD5=md5; fi
HERE=$(cd "$(dirname "$0")" && pwd)
LVGL=${LVGL_PATH:-$HERE/../../../../managed_components/lvgl__lvgl}
CMP=$(cd "$HERE/../.." && pwd)
OBJ=${OBJ_DIR:-/tmp/facehost-obj}

if [ ! -f "$LVGL/lvgl.h" ]; then
    echo "LVGL not found at $LVGL (set LVGL_PATH)" >&2
    exit 1
fi
mkdir -p "$OBJ" "$HERE/out"

if [ ! -f "$OBJ/libfacehost-lvgl.a" ]; then
    echo "compiling LVGL from $LVGL (one-off, ~1 min)..."
    find "$LVGL/src" -name '*.c' > "$OBJ/srcs.txt"
    rm -f "$OBJ/err.log"
    # per-file compile helper, so paths with spaces/quoting stay simple
    cat > "$OBJ/cc1.sh" <<EOF
#!/bin/sh
gcc -c -O1 -I"$HERE" -I"$LVGL" -I"$CMP/include" -DLV_CONF_INCLUDE_SIMPLE \\
    -o "$OBJ/\$(echo "\$1" | $MD5).o" "\$1" 2>>"$OBJ/err.log" ||
    echo "FAIL \$1" >> "$OBJ/err.log"
EOF
    chmod +x "$OBJ/cc1.sh"
    xargs -n1 -P8 "$OBJ/cc1.sh" < "$OBJ/srcs.txt"
    if grep -q '^FAIL' "$OBJ/err.log" 2>/dev/null; then
        echo "LVGL compile failures:" >&2
        grep '^FAIL' "$OBJ/err.log" >&2
        exit 1
    fi
    ar rcs "$OBJ/libfacehost-lvgl.a" "$OBJ"/*.o
fi

gcc -O1 -include string.h -I"$HERE" -I"$LVGL" -I"$CMP/include" \
    -DLV_CONF_INCLUDE_SIMPLE -o "$HERE/facehost" \
    "$HERE/main.c" "$CMP/lvgl_kawaii_face.c" "$OBJ/libfacehost-lvgl.a" -lm

cd "$HERE" && ./facehost
