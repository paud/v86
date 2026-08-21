#!/bin/sh
set -eu

output_dir=${1:-/private/tmp/d3d8-stage4-tests}
compiler=${D8WG_CC:-i686-w64-mingw32-gcc}
mkdir -p "$output_dir"

for test_name in \
    transform_depth \
    raster_stencil \
    lighting \
    fog \
    textured_cube
do
    "$compiler" -mwindows -std=gnu99 -Os -s -nostdlib \
        -Wall -Wextra -Werror \
        -Wl,--subsystem,windows:5.01 -Wl,-e,_WinMainCRTStartup@0 \
        -o "$output_dir/d3d8_${test_name}_test.exe" \
        "glbridge/sample/d3d8_${test_name}_test.c" \
        -ld3d8 -lgdi32 -luser32 -lkernel32
done

echo "Built D3D8 Stage 4 tests in $output_dir"
