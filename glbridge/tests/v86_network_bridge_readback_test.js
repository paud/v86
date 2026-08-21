"use strict";

const assert = require("node:assert/strict");

require("../v86_network_bridge.js");

const GLFN_READ_PIXELS = 94;
const GL_RGBA = 0x1908;
const GL_UNSIGNED_BYTE = 0x1401;

function makeModule() {
    let nextPtr = 256;
    return {
        HEAPU8: new Uint8Array(4096),
        _malloc(size) {
            const ptr = nextPtr;
            nextPtr += Math.max(size, 4);
            return ptr;
        },
        _free() {},
        _v86glResize() {},
        _v86gl_glReadPixels(x, y, width, height, format, type, ptr) {
            assert.deepEqual([x, y, width, height, format, type],
                [0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE]);
            /* Model PACK_SKIP_PIXELS=1: the backend writes one pixel after
             * four untouched bytes in the supplied output span. */
            this.HEAPU8.set([0x11, 0x22, 0x33, 0x44], ptr + 4);
            return 1;
        },
    };
}

const moduleObject = makeModule();
const canvas = {
    width: 64,
    height: 64,
    style: {},
    parentElement: { getElementsByTagName() { return [canvas]; } },
};
const bridge = globalThis.installV86GLNetworkBridge(
    { add_listener() {} }, canvas, { gl4es: moduleObject });
const payload = Buffer.alloc(32 + 12, 0xA5);
payload.writeInt32LE(0, 0);
payload.writeInt32LE(0, 4);
payload.writeInt32LE(1, 8);
payload.writeInt32LE(1, 12);
payload.writeUInt32LE(GL_RGBA, 16);
payload.writeUInt32LE(GL_UNSIGNED_BYTE, 20);
payload.writeUInt32LE(12, 24);
payload.writeUInt32LE(0, 28);

bridge.renderer.glCall(GLFN_READ_PIXELS, payload);
assert.equal(payload.readUInt32LE(28), 1, "readback must complete synchronously");
assert.deepEqual(Array.from(payload.subarray(32)), [
    0xA5, 0xA5, 0xA5, 0xA5,
    0x11, 0x22, 0x33, 0x44,
    0xA5, 0xA5, 0xA5, 0xA5,
], "PACK skip and padding bytes must remain unchanged");

console.log("v86_network_bridge_readback_test: ok");
