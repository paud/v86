#!/usr/bin/env bash
# v86 build script
# Usage:
#   ./build.sh          # debug build (WASM only, no closure)
#   ./build.sh release  # release build (WASM + closure JS)
#   ./build.sh clean    # clean build artifacts

set -e

cd "$(dirname "$0")"

# Ensure cargo is in PATH
export PATH="$HOME/.cargo/bin:$PATH"

MODE="${1:-debug}"

build_c_objects() {
    echo "==> Compiling C object files..."
    mkdir -p build

    clang -c -Wall \
        --target=wasm32 -O3 -flto -nostdlib \
        -fvisibility=hidden -ffunction-sections -fdata-sections \
        -DSOFTFLOAT_FAST_INT64 -DINLINE_LEVEL=5 \
        -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 \
        -o build/softfloat.o \
        lib/softfloat/softfloat.c

    clang -c -Wall \
        --target=wasm32 -O3 -flto -nostdlib \
        -fvisibility=hidden -ffunction-sections -fdata-sections \
        -DZSTDLIB_VISIBILITY="" \
        -o build/zstddeclib.o \
        lib/zstd/zstddeclib.c

    echo "    done."
}

generate_tables() {
    echo "==> Generating instruction tables..."
    node gen/generate_jit.js --all
    node gen/generate_interpreter.js --all
    node gen/generate_analyzer.js --all
    echo "    done."
}

build_wasm_debug() {
    echo "==> Building v86-debug.wasm (debug)..."
    cargo rustc --target wasm32-unknown-unknown -- \
        -C linker=tools/rust-lld-wrapper \
        -C link-args="--import-table --global-base=4096" \
        -C link-args="build/softfloat.o" \
        -C link-args="build/zstddeclib.o" \
        --verbose
    cp build/wasm32-unknown-unknown/debug/v86.wasm build/v86-debug.wasm
    # Also copy as v86.wasm for non-DEBUG mode
    cp build/v86-debug.wasm build/v86.wasm
    echo "    done: $(ls -lh build/v86-debug.wasm | awk '{print $5}')"
}

build_wasm_release() {
    echo "==> Building v86.wasm (release)..."
    cargo rustc --release --target wasm32-unknown-unknown -- \
        -C linker=tools/rust-lld-wrapper \
        -C link-args="--import-table --global-base=4096" \
        -C link-args="build/softfloat.o" \
        -C link-args="build/zstddeclib.o" \
        -C target-feature=+bulk-memory \
        -C target-feature=+multivalue \
        -C target-feature=+simd128 \
        --verbose
    cp build/wasm32-unknown-unknown/release/v86.wasm build/v86.wasm
    echo "    done: $(ls -lh build/v86.wasm | awk '{print $5}')"
}

build_js() {
    echo "==> Building v86_all.js (esbuild)..."

    npx -y esbuild src/browser/main.js \
        --bundle \
        --format=iife \
        --global-name=V86 \
        --outfile=build/v86_all.js \
        --define:DEBUG=false \
        --platform=browser \
        --external:perf_hooks \
        --external:crypto \
        --external:node:crypto \
        --external:./capstone-x86.min.js \
        --external:./libwabt.cjs \
        --external:node:fs/promises \
        --external:fs \
        --external:path \
        --external:url \
        --minify

    echo "    done: $(ls -lh build/v86_all.js | awk '{print $5}')"
}

clean() {
    echo "==> Cleaning..."
    rm -rf build/
    rm -f src/rust/gen/jit.rs src/rust/gen/jit0f.rs
    rm -f src/rust/gen/interpreter.rs src/rust/gen/interpreter0f.rs
    rm -f src/rust/gen/analyzer.rs src/rust/gen/analyzer0f.rs
    cargo clean
    echo "    done."
}

case "$MODE" in
    debug)
        generate_tables
        build_c_objects
        build_wasm_debug
        echo ""
        echo "==> Debug build complete."
        echo "    Open http://localhost:8082/dev.html?profile=windows98"
        ;;
    release)
        generate_tables
        build_c_objects
        build_wasm_release
        build_js
        echo ""
        echo "==> Release build complete."
        echo "    Open http://localhost:8082/?profile=windows98"
        ;;
    clean)
        clean
        ;;
    *)
        echo "Usage: $0 [debug|release|clean]"
        echo ""
        echo "  debug    Build WASM (debug) — use with dev.html"
        echo "  release  Build WASM (release) + v86_all.js — use with index.html"
        echo "  clean    Remove all build artifacts"
        exit 1
        ;;
esac
