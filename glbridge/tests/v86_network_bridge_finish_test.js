"use strict";

const assert = require("node:assert/strict");

require("../v86_network_bridge.js");

const GLFN_FINISH = 9;
const V86GL_SYNC_QUERY_STATUS_PENDING = 0;
const V86GL_SYNC_QUERY_STATUS_OK = 1;
const V86GL_SYNC_QUERY_STATUS_FAILED = 2;

function install(moduleObject) {
    const listeners = Object.create(null);
    const canvas = {
        width: 64,
        height: 64,
        style: {},
        parentElement: { getElementsByTagName() { return [canvas]; } },
    };
    const bridge = globalThis.installV86GLNetworkBridge({
        add_listener(name, callback) { listeners[name] = callback; },
    }, canvas, { gl4es: moduleObject });
    return { bridge, listeners };
}

let finishCalls = 0;
const success = install({
    HEAPU8: new Uint8Array(4096),
    _malloc() { return 256; },
    _free() {},
    _v86glResize() {},
    _v86gl_glFinish() { finishCalls++; return 1; },
});
const successBridge = success.bridge;
const successPayload = Buffer.alloc(4);
successPayload.writeUInt32LE(V86GL_SYNC_QUERY_STATUS_PENDING);
successBridge.renderer.glCall(GLFN_FINISH, successPayload);
assert.equal(finishCalls, 1, "glFinish must reach the gl4es export");
assert.equal(successPayload.readUInt32LE(0), V86GL_SYNC_QUERY_STATUS_OK,
    "a completed host glFinish must acknowledge success to the guest");

/* Exercise the actual PCI record decoder so the ACK is proven to mutate the
 * shared command payload, not a temporary copy. */
const finishRecord = Buffer.alloc(8);
finishRecord.writeUInt16LE(GLFN_FINISH, 0);
finishRecord.writeUInt16LE(4, 2);
finishRecord.writeUInt32LE(V86GL_SYNC_QUERY_STATUS_PENDING, 4);
success.listeners["v86gl-pci-frame"]({
    bytes: finishRecord,
    frameId: 1,
    submitCount: 1,
    commandCount: 1,
    flags: 0,
});
assert.equal(finishCalls, 2);
assert.equal(finishRecord.readUInt32LE(4), V86GL_SYNC_QUERY_STATUS_OK,
    "the PCI decoder must update the original mapped payload in place");

const failureBridge = install({
    HEAPU8: new Uint8Array(4096),
    _malloc() { return 256; },
    _free() {},
    _v86glResize() {},
    _v86gl_glFinish() { return 0; },
}).bridge;
const failurePayload = Buffer.alloc(4);
failurePayload.writeUInt32LE(V86GL_SYNC_QUERY_STATUS_PENDING);
failureBridge.renderer.glCall(GLFN_FINISH, failurePayload);
assert.equal(failurePayload.readUInt32LE(0), V86GL_SYNC_QUERY_STATUS_FAILED,
    "an unready host glFinish must be visible to the guest");

/* Payload-less records from an older proxy remain accepted. */
successBridge.renderer.glCall(GLFN_FINISH, Buffer.alloc(0));
assert.equal(finishCalls, 3);

console.log("v86_network_bridge_finish_test: ok");
