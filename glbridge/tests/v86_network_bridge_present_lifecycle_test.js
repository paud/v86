"use strict";

const assert = require("node:assert/strict");

require("../v86_network_bridge.js");

const GLFN_CLEAR = 3;
const GL_COLOR_BUFFER_BIT = 0x00004000;

function glRecord(fn, payload) {
    const record = Buffer.alloc(4 + payload.length);
    record.writeUInt16LE(fn, 0);
    record.writeUInt16LE(payload.length, 2);
    payload.copy(record, 4);
    return record;
}

function u32Payload(value) {
    const payload = Buffer.alloc(4);
    payload.writeUInt32LE(value >>> 0, 0);
    return payload;
}

function install(presentResult) {
    const listeners = Object.create(null);
    const calls = [];
    const style = {
        setProperty(name, value) {
            this[name] = value;
        },
    };
    const canvas = { width: 640, height: 480, style, parentElement: null };
    const module = {
        HEAPU8: new Uint8Array(4096),
        _malloc() { return 256; },
        _free() {},
        _v86glResize() {},
        _v86gl_glClear(mask) { calls.push(["clear", mask]); },
        _v86glReleaseCurrent() { calls.push(["releaseCurrent"]); },
        _v86glPresent() {
            calls.push(["present"]);
            return presentResult.value;
        },
    };
    const emulator = {
        add_listener(name, callback) {
            listeners[name] = callback;
        },
    };
    const bridge = globalThis.installV86GLNetworkBridge(
        emulator, canvas, { gl4es: module });
    return { bridge, calls, listeners, canvas };
}

const successfulResult = { value: 1 };
const success = install(successfulResult);
const clear = glRecord(GLFN_CLEAR, u32Payload(GL_COLOR_BUFFER_BIT));

success.listeners["v86gl-pci-frame"]({
    bytes: clear,
    frameId: 1,
    submitCount: 1,
    commandCount: 1,
    flags: 0,
});

const emptyPresent = {
    bytes: Buffer.alloc(0),
    frameId: 1,
    submitCount: 2,
    commandCount: 0,
    flags: 1,
};
success.listeners["v86gl-pci-frame"](emptyPresent);
assert.equal(success.bridge.lastPresentedFrameId, 1,
    "a successful commit must advance the presented frame id");
assert.equal(success.bridge.overlayVisible, true,
    "drawable state must survive non-present batches until the empty Present batch");
assert.deepEqual(success.calls.slice(0, 2), [
    ["clear", GL_COLOR_BUFFER_BIT],
    ["present"],
]);

success.bridge.releaseCurrent();
assert.equal(success.bridge.overlayVisible, true,
    "releasing the WGL current context must preserve the committed front buffer");

const emptySecondPresent = {
    bytes: Buffer.alloc(0),
    frameId: 2,
    submitCount: 3,
    commandCount: 0,
    flags: 1,
};
success.listeners["v86gl-pci-frame"](emptySecondPresent);
assert.equal(success.bridge.overlayVisible, true,
    "an empty swap must not hide a previously committed drawable");

const failedResult = { value: 0 };
const failure = install(failedResult);
failure.listeners["v86gl-pci-frame"]({
    bytes: clear,
    frameId: 7,
    submitCount: 1,
    commandCount: 1,
    flags: 0,
});
const failedPresent = {
    bytes: Buffer.alloc(0),
    frameId: 7,
    submitCount: 2,
    commandCount: 0,
    flags: 1,
};
failure.listeners["v86gl-pci-frame"](failedPresent);
assert.equal(failure.bridge.lastPresentedFrameId, 0,
    "a failed commit must not consume the frame id");
assert.equal(failure.bridge.overlayVisible, false,
    "a failed commit must not expose an uncommitted back buffer");
assert.equal(failure.bridge.frameStates[7].drawable, true,
    "a failed frame must remain available for a same-id retry");

failedResult.value = 1;
failure.listeners["v86gl-pci-frame"](failedPresent);
assert.equal(failure.bridge.lastPresentedFrameId, 7);
assert.equal(failure.bridge.overlayVisible, true);

console.log("v86_network_bridge_present_lifecycle_test: ok");
