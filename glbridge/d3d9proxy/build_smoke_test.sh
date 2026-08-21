#!/bin/sh
set -eu
# Run from the repository root, e.g.:
#   ./glbridge/d3d9proxy/build_smoke_test.sh /private/tmp/d3d9-smoke-test
# (mirrors ../d3d8proxy/build_stage3_tests.sh's invocation convention).

output_dir=${1:-/private/tmp/d3d9-smoke-test}
compiler=${D9WG_CC:-i686-w64-mingw32-gcc}
mkdir -p "$output_dir"

for test_name in \
    clear \
    triangle \
    texture \
    world \
    reset \
    shader \
    swapchain \
    surface_lifetime
do
    "$compiler" -mwindows -std=gnu99 -Os -s -nostdlib \
        -Wall -Wextra -Werror \
        -Wl,--subsystem,windows:5.01 -Wl,-e,_WinMainCRTStartup@0 \
        -o "$output_dir/d3d9_${test_name}_test.exe" \
        "glbridge/sample/d3d9_${test_name}_test.c" \
        -ld3d9 -lgdi32 -luser32 -lkernel32
done

echo "Built D3D9 smoke test(s) in $output_dir"
echo "Copy the current d3d9.dll (see glbridge/d3d9proxy/build.sh) beside each .exe before running."
