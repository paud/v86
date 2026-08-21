#!/bin/sh
set -eu

output_dir=${1:-/private/tmp/d3d8-stage6-tests}
compiler=${D8WG_CC:-i686-w64-mingw32-gcc}
mkdir -p "$output_dir"

# d3d8_caps_audit_test.c now doubles as the Stage 6 acceptance executable:
# audit_caps() requires VertexShaderVersion/PixelShaderVersion to be honestly
# advertised as vs_1_1/ps_1_4, audit_shader_pipeline() creates and draws with
# a real VS1.1 + PS1.1 pair, and audit_unsupported_shader_bytecode_rejected()
# confirms a wrong-version or unsupported-opcode shader is still rejected
# rather than silently mistranslated. Per-instruction numeric coverage for
# the VS1.1/PS1.1-1.4 translator lives in the host executor test suite
# (glbridge/tests/d3d8_webgpu_executor_test.js), where each instruction can
# be checked deterministically without an XP/WebGPU environment.
for test_name in \
    caps_audit
do
    "$compiler" -mwindows -std=gnu99 -Os -s -nostdlib \
        -Wall -Wextra -Werror \
        -Wl,--subsystem,windows:5.01 -Wl,-e,_WinMainCRTStartup@0 \
        -o "$output_dir/d3d8_${test_name}_test.exe" \
        "glbridge/sample/d3d8_${test_name}_test.c" \
        -ld3d8 -lgdi32 -luser32 -lkernel32
done

echo "Built D3D8 Stage 6 tests in $output_dir"
